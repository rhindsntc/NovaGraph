import XCTest
import GraphDBKit
@testable import NovaSample

@MainActor final class SampleModelTests: XCTestCase {
    func location() -> URL { FileManager.default.temporaryDirectory.appendingPathComponent("nova-sample-test-" + UUID().uuidString) }

    func testFirstInstallEditsTypedPathsAndReopenWithoutReseeding() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        XCTAssertEqual(model.state, .ready); XCTAssertTrue(model.nodes.isEmpty)
        await model.perform(.save("ada", "Ada's edited name")).value
        await model.perform(.save("grace", "Grace")).value
        await model.perform(.connect("ada", "grace")).value
        XCTAssertNil(model.error)
        await model.perform(.paths("ada")).value
        XCTAssertNil(model.error)
        XCTAssertEqual(model.paths.map { $0.nodes.map(\.id) }, [["ada", "grace"]])
        XCTAssertEqual(model.paths.first?.edges.first?.type, "KNOWS")
        await model.perform(.close).value
        XCTAssertEqual(model.state, .closed)
        await model.perform(.open).value
        XCTAssertEqual(model.nodes.first { $0.id == "ada" }?["name"], .string("Ada's edited name"))
        await model.perform(.close).value
    }

    func testRejectedTransactionShowsErrorAndDoesNotPublishPartialWrite() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        await model.perform(.query("upsert node Person partial set name=\"No\"; delete node missing")).value
        XCTAssertNotNil(model.error)
        await model.perform(.query("find nodes Person")).value
        XCTAssertTrue(model.nodes.isEmpty)
        XCTAssertNil(model.error)
        await model.perform(.close).value
    }

    func testCancelledTraversalKeepsLastResultAndAllowsNextRead() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        await model.perform(.save("ada", "Ada")).value
        let operation = model.perform(.paths("ada")); model.cancel()
        await operation.value
        XCTAssertEqual(model.status, "Cancelled"); XCTAssertFalse(model.busy)
        await model.perform(.query("find nodes Person")).value
        XCTAssertNil(model.error); XCTAssertEqual(model.nodes.map(\.id), ["ada"])
        await model.perform(.close).value
    }

    func testPressureAndBackgroundForegroundPreserveGraph() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        await model.perform(.save("ada", "Ada")).value
        await model.perform(.trim).value
        XCTAssertTrue(model.status.contains("Evicted 1"), model.status)
        await model.lifecycle(.background).value
        XCTAssertEqual(model.state, .suspended)
        await model.lifecycle(.memoryPressure).value
        XCTAssertEqual(model.state, .suspended)
        await model.lifecycle(.foreground).value
        XCTAssertEqual(model.state, .ready)
        await model.perform(.query("find nodes Person")).value
        XCTAssertEqual(model.nodes.map(\.id), ["ada"])
        await model.perform(.close).value
    }

    func testUnavailableStorageIsVisibleAndNeverReplaced() async throws {
        let path = location(); try Data("keep".utf8).write(to: path)
        defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        XCTAssertEqual(model.state, .failed); XCTAssertNotNil(model.error)
        XCTAssertEqual(try String(contentsOf: path, encoding: .utf8), "keep")
        await model.perform(.close).value
    }

    func testCloseDuringOpenDrainsOwnershipAndAllowsAnotherOwner() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        let opening = model.perform(.open)
        await model.perform(.close).value
        await opening.value
        XCTAssertEqual(model.state, .closed); XCTAssertFalse(model.busy)
        let other = try GraphDatabase(path: path)
        try await other.close()
    }

    func testCancelledAcquiredOpenClosesWithoutInheritingCancellation() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let gate = OpenGate()
        let model = SampleModel(path: path, openDatabase: { path in
            let database = try await DatabaseOpener().open(path)
            await gate.pause()
            return database
        })
        let opening = model.perform(.open)
        await gate.waitUntilPaused()
        model.cancel()
        await gate.release()
        await opening.value
        XCTAssertEqual(model.state, .closed)
        XCTAssertNil(model.error)
        let nextOwner = try GraphDatabase(path: path)
        try await nextOwner.close()
    }

    func testFailedSuspendKeepsResumeAvailableAndRecovers() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        await model.perform(.save("ada", "Ada")).value
        try blockCheckpoint(path)
        await model.lifecycle(.background).value
        XCTAssertEqual(model.state, .suspended)
        XCTAssertNotNil(model.error)
        try restoreCheckpoint(path)
        await model.lifecycle(.foreground).value
        XCTAssertEqual(model.state, .ready); XCTAssertNil(model.error)
        await model.perform(.query("find nodes Person")).value
        XCTAssertEqual(model.nodes.map(\.id), ["ada"])
        await model.perform(.close).value
    }

    func testFailedCloseAllowsRecoveryAndAcknowledgement() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        await model.perform(.save("ada", "Ada")).value
        try blockCheckpoint(path)
        await model.perform(.close).value
        XCTAssertEqual(model.state, .failed); XCTAssertNotNil(model.error)
        try restoreCheckpoint(path)
        await model.perform(.open).value
        XCTAssertEqual(model.state, .ready); XCTAssertNil(model.error)
        XCTAssertEqual(model.nodes.map(\.id), ["ada"])
        try blockCheckpoint(path)
        await model.perform(.close).value
        XCTAssertEqual(model.state, .failed); XCTAssertNotNil(model.error)
        try restoreCheckpoint(path)
        // A second explicit close acknowledges the reported terminal failure.
        await model.perform(.close).value
        XCTAssertEqual(model.state, .closed)
    }

    func testSuspendDrainTimeoutKeepsResumeAvailable() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let database = try GraphDatabase(path: path)
        let model = SampleModel(path: path, openDatabase: { _ in database })
        await model.perform(.open).value
        let began = OpenGate(), release = DispatchSemaphore(value: 0)
        let reading = Task.detached {
            try database.readSnapshot { _ in
                Task { await began.pause() }
                _ = release.wait(timeout: .now() + 15)
            }
        }
        await began.waitUntilPaused()
        await model.lifecycle(.background).value
        XCTAssertEqual(model.state, .suspended); XCTAssertNotNil(model.error)
        release.signal(); await began.release(); _ = await reading.result
        await model.lifecycle(.foreground).value
        XCTAssertEqual(model.state, .ready); XCTAssertNil(model.error)
        await model.perform(.close).value
    }

    func testCloseDrainTimeoutRetainsOwnerUntilRetry() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let database = try GraphDatabase(path: path)
        let model = SampleModel(path: path, openDatabase: { _ in database })
        await model.perform(.open).value
        let began = OpenGate(), release = DispatchSemaphore(value: 0)
        let reading = Task.detached {
            try database.readSnapshot { _ in
                Task { await began.pause() }
                _ = release.wait(timeout: .now() + 15)
            }
        }
        await began.waitUntilPaused()
        await model.perform(.close).value
        XCTAssertEqual(model.state, .failed); XCTAssertNotNil(model.error)
        XCTAssertThrowsError(try GraphDatabase(path: path))
        release.signal(); await began.release(); _ = await reading.result
        await model.perform(.close).value
        XCTAssertEqual(model.state, .closed)
        let nextOwner = try GraphDatabase(path: path)
        try await nextOwner.close()
    }

    private func blockCheckpoint(_ path: URL) throws {
        try FileManager.default.moveItem(at: path.appendingPathComponent("CURRENT"), to: path.appendingPathComponent("saved-current"))
        try FileManager.default.createDirectory(at: path.appendingPathComponent("CURRENT"), withIntermediateDirectories: false)
    }
    private func restoreCheckpoint(_ path: URL) throws {
        try FileManager.default.removeItem(at: path.appendingPathComponent("CURRENT"))
        try FileManager.default.moveItem(at: path.appendingPathComponent("saved-current"), to: path.appendingPathComponent("CURRENT"))
    }

    func testUserIdentifiersAreQuotedRatherThanExecuted() async throws {
        let path = location(); defer { try? FileManager.default.removeItem(at: path) }
        let model = SampleModel(path: path)
        await model.perform(.open).value
        let id = "id\"; delete node ada; //"
        await model.perform(.save(id, "quoted \" name")).value
        XCTAssertNil(model.error); XCTAssertEqual(model.nodes.map(\.id), [id])
        await model.perform(.close).value
    }
}

private actor OpenGate {
    private var paused = false
    private var observers: [CheckedContinuation<Void, Never>] = []
    private var continuation: CheckedContinuation<Void, Never>?
    func pause() async {
        paused = true
        observers.forEach { $0.resume() }; observers = []
        await withCheckedContinuation { continuation = $0 }
    }
    func waitUntilPaused() async {
        if paused { return }
        await withCheckedContinuation { observers.append($0) }
    }
    func release() { continuation?.resume(); continuation = nil }
}
