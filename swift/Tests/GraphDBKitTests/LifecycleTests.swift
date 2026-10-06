import XCTest
import Foundation
@testable import GraphDBKit

private final class StartGate: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Void, Never>?
    func wait(entered: XCTestExpectation) async {
        await withCheckedContinuation { value in
            lock.lock(); continuation = value; lock.unlock(); entered.fulfill()
        }
    }
    func release() {
        lock.lock(); let value = continuation; continuation = nil; lock.unlock(); value?.resume()
    }
}

final class LifecycleTests: XCTestCase, @unchecked Sendable {
    private func cleanup(_ db: GraphDatabase, _ path: URL) { try? db.close(); try? FileManager.default.removeItem(at: path) }
    private func database() throws -> (GraphDatabase, URL) {
        let path = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        return (try GraphDatabase(path: path), path)
    }
    func testAlreadyCancelledAsyncMutationAppliesNothing() async throws {
        let (db, path) = try database(); defer { cleanup(db, path) }
        let entered = expectation(description: "task reached barrier"), gate = StartGate()
        let task = Task {
            await gate.wait(entered: entered)
            return try await db.execute("upsert node N cancelled")
        }
        await fulfillment(of: [entered], timeout: 2)
        task.cancel(); gate.release()
        do { _ = try await task.value; XCTFail("Cancelled write was acknowledged") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "cancelled") }
        let absent: Node? = try await db.node(id: "cancelled")
        XCTAssertNil(absent)
    }
    func testExplicitCloseReleasesDirectoryAndRejectsQueries() throws {
        let (db, path) = try database(); defer { cleanup(db, path) }
        try db.upsertNode(label: "N", id: "kept")
        try db.close(); try db.close()
        XCTAssertThrowsError(try db.node(id: "kept"))
        let reopened = try GraphDatabase(path: path)
        defer { try? reopened.close() }
        XCTAssertNotNil(try reopened.node(id: "kept"))
    }
    func testSnapshotRejectsDatabaseReentrancyAndEscapedUse() throws {
        let (db, path) = try database(); defer { cleanup(db, path) }
        try db.upsertNode(label: "N", id: "a")
        var escaped: GraphSnapshot?
        try db.readSnapshot { view in
            escaped = view
            XCTAssertNotNil(try view.node(id: "a"))
            XCTAssertThrowsError(try db.upsertNode(label: "N", id: "b"))
        }
        XCTAssertThrowsError(try escaped?.node(id: "a"))
    }
}


extension LifecycleTests {
    @MainActor func testLifecycleEventsAreOrderedAndPressureReclaimsPayloads() async throws {
        let (db, path) = try database()
        try await db.execute("upsert node N a")
        let lifecycle = GraphLifecycle(database: db)
        try await lifecycle.send(.background).value
        do { _ = try await db.node(id: "a"); XCTFail("Suspended database accepted a read") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "busy") }
        try await lifecycle.send(.memoryPressure).value
        do { _ = try await db.node(id: "a"); XCTFail("Pressure trim reopened query admission") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "busy") }
        // Foreground immediately follows background; execution must stay ordered.
        let background = lifecycle.send(.background)
        let foreground = lifecycle.send(.foreground)
        try await foreground.value; try await background.value
        XCTAssertNotNil(try awaitNode(db))
        try await lifecycle.send(.memoryPressure).value
        let trim = try await db.trimMemory()
        XCTAssertEqual(trim.evicted, 0, "Pressure handler should have evicted the payload")
        try await db.close(); try FileManager.default.removeItem(at: path)
    }
    private func awaitNode(_ db: GraphDatabase) throws -> Node? { try db.node(id: "a") }

    func testCancellationDuringSnapshotBodyIsReportedAfterItReturns() async throws {
        let (db, path) = try database()
        let entered = expectation(description: "snapshot entered")
        let release = DispatchSemaphore(value: 0)
        let task = Task {
            try await db.readSnapshot { _ in
                entered.fulfill()
                _ = release.wait(timeout: .now() + 2)
                return 42
            }
        }
        await fulfillment(of: [entered], timeout: 2)
        task.cancel(); release.signal()
        do { _ = try await task.value; XCTFail("Cancelled snapshot returned success") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "cancelled") }
        try await db.close(); try FileManager.default.removeItem(at: path)
    }
    func testReadDecodeChecksCancellationWithoutMaskingCommittedOutcomes() throws {
        let token = try GraphRequest(); token.cancel()
        let read = Data(#"{"schemaVersion":2,"ok":true,"data":[]}"#.utf8)
        XCTAssertThrowsError(try decodeV2(read, request: token)) { error in
            XCTAssertEqual((error as? GraphCoreError)?.code, "cancelled")
        }
        let committed = Data(#"{"schemaVersion":2,"ok":true,"data":{},"receipt":{"transactionId":"abc","committedLSN":3}}"#.utf8)
        XCTAssertEqual(try decodeV2(committed, request: token).receipt?.committedLSN, 3)
        let unknown = Data(#"{"schemaVersion":2,"ok":false,"error":{"code":"commitOutcomeUnknown","message":"sync failed","context":{"transactionId":"abc"}}}"#.utf8)
        XCTAssertThrowsError(try decodeV2(unknown, request: token)) { error in
            XCTAssertEqual((error as? GraphCoreError)?.code, "commitOutcomeUnknown")
        }
    }
}


extension LifecycleTests {
    // Keep overload selection explicit inside async tests.
    private func checkpointSynchronously(_ db: GraphDatabase) throws { try db.checkpoint() }
    private func closeSynchronously(_ db: GraphDatabase) { try? db.close() }

    func testCheckpointOverloadsBypassQueryWorkspaceAndSurviveReopen() async throws {
        let (seed, path) = try database()
        defer { try? FileManager.default.removeItem(at: path) }
        try seed.upsertNode(label: "N", id: "kept", properties: ["value": .string(String(repeating: "x", count: 4096))])
        try await seed.close()
        var configuration = GraphDatabaseConfiguration(path: path)
        configuration.queryOptions.workingBytes = 1024
        let db = try GraphDatabase(configuration: configuration)
        defer { closeSynchronously(db) }
        try checkpointSynchronously(db)
        do { try await db.checkpoint() }
        catch { XCTFail("Async maintenance checkpoint failed: \(error)"); return }
        do { _ = try await db.queryResult("checkpoint"); XCTFail("NGQL checkpoint ignored its query budget") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "limitExceeded") }
        try await db.close()
        let reopened = try GraphDatabase(path: path)
        defer { closeSynchronously(reopened) }
        let node = try await reopened.node(id: "kept")
        XCTAssertEqual(node?.properties["value"], .string(String(repeating: "x", count: 4096)))
    }

    func testAlreadyCancelledAsyncCheckpointDoesNotRun() async throws {
        let (db, path) = try database(); defer { cleanup(db, path) }
        let entered = expectation(description: "checkpoint reached barrier"), gate = StartGate()
        let task = Task {
            await gate.wait(entered: entered)
            try await db.checkpoint()
        }
        await fulfillment(of: [entered], timeout: 2)
        task.cancel(); gate.release()
        do { try await task.value; XCTFail("Cancelled checkpoint succeeded") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "cancelled") }
    }

    func testAsyncCheckpointReportsIOFailure() async throws {
        let (db, path) = try database(); defer { cleanup(db, path) }
        let current = path.appendingPathComponent("CURRENT")
        let saved = path.appendingPathComponent("saved-current")
        try FileManager.default.moveItem(at: current, to: saved)
        try FileManager.default.createDirectory(at: current, withIntermediateDirectories: false)
        defer {
            try? FileManager.default.removeItem(at: current)
            try? FileManager.default.moveItem(at: saved, to: current)
        }
        do { try await db.checkpoint(); XCTFail("Checkpoint hid the IO failure") }
        catch let error as GraphCoreError { XCTAssertEqual(error.code, "ioFailure") }
    }
}
