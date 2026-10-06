import Foundation
import XCTest
import GraphDBKit
import NIO
import NIOHTTP1
import NIOPosix
@testable import NovaDevQuery

private final class ResponseCollector: ChannelInboundHandler, @unchecked Sendable {
    typealias InboundIn = ByteBuffer
    let received = DispatchSemaphore(value: 0)
    let closed = XCTestExpectation(description: "connection closed")
    private let lock = NSLock()
    private var bytes = Data()
    var text: String { lock.lock(); defer { lock.unlock() }; return String(decoding: bytes, as: UTF8.self) }
    func channelRead(context: ChannelHandlerContext, data: NIOAny) {
        lock.lock(); bytes.append(contentsOf: unwrapInboundIn(data).readableBytesView); lock.unlock(); received.signal()
    }
    func channelInactive(context: ChannelHandlerContext) { closed.fulfill() }
}

private final class DelayedCommittedResponse {
    let root: URL, session: RunnerSession
    let pool = NIOThreadPool(numberOfThreads: 1)
    let group = MultiThreadedEventLoopGroup(numberOfThreads: 1)
    let committed = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
    let server: Channel
    var drained = false
    init(timeout: TimeAmount) throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("nova-http-outcome-\(UUID())")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try Data("<html></html>".utf8).write(to: root.appendingPathComponent("index.html"))
        session = try RunnerSession(port: 1234, webRoot: root, database: nil)
        pool.start()
        let session = session, pool = pool, committed = committed, release = release
        server = try ServerBootstrap(group: group).childChannelInitializer { channel in
            guard session.admitConnection() else { return channel.close() }
            return channel.pipeline.configureHTTPServerPipeline(withPipeliningAssistance: false).flatMap {
                channel.pipeline.addHandler(DevQueryHandler(session: session, pool: pool, requestTimeout: timeout) { path, body, request in
                    let response = session.execute(path: path, body: body, request: request)
                    // The actual engine has already returned its durable receipt. Hold
                    // delivery to exercise the same transport race as a slow commit.
                    committed.signal(); _ = release.wait(timeout: .now() + 5)
                    return response
                })
            }
        }.bind(host: "127.0.0.1", port: 0).wait()
    }
    func connect(_ collector: ResponseCollector) throws -> Channel {
        try ClientBootstrap(group: group).channelInitializer { $0.pipeline.addHandler(collector) }
            .connect(to: server.localAddress!).wait()
    }
    func send(_ query: String, to channel: Channel) throws {
        let body = String(decoding: try JSONEncoder().encode(["query":query]), as: UTF8.self)
        let head = "POST /dev/query HTTP/1.1\r\nHost: 127.0.0.1:1234\r\nOrigin: http://127.0.0.1:1234\r\nX-Nova-Session: \(session.token)\r\nContent-Type: application/json\r\nContent-Length: \(body.utf8.count)\r\n\r\n"
        var bytes = channel.allocator.buffer(capacity: head.utf8.count + body.utf8.count)
        bytes.writeString(head + body); try channel.writeAndFlush(bytes).wait()
    }
    func drain() throws { if !drained { release.signal(); try pool.syncShutdownGracefully(); drained = true } }
    func read(_ id: String) throws -> String {
        try drain()
        let body = try JSONEncoder().encode(["query":"get node \(id)"])
        return String(decoding: session.execute(path: "/dev/query", body: body, request: try GraphRequest()).body, as: UTF8.self)
    }
    deinit {
        try? drain(); try? server.close().wait(); try? group.syncShutdownGracefully(); try? session.close()
        try? FileManager.default.removeItem(at: root)
    }
}

final class OutcomeTests: XCTestCase {
    func testDeadlineAfterDurableCommitReportsUnknownOutcome() throws {
        let harness = try DelayedCommittedResponse(timeout: .milliseconds(200)), collector = ResponseCollector()
        let channel = try harness.connect(collector)
        try harness.send("upsert node N committed", to: channel)
        XCTAssertEqual(harness.committed.wait(timeout: .now() + 2), .success)
        wait(for: [collector.closed], timeout: 2)
        XCTAssertTrue(collector.text.contains("outcomeUnknown"), collector.text)
        XCTAssertTrue(collector.text.contains("may have committed"), collector.text)
        XCTAssertTrue(try harness.read("committed").contains("\"id\":\"committed\""))
    }
    func testPipelinedRequestCannotOverwriteCommittedReceipt() throws {
        let harness = try DelayedCommittedResponse(timeout: .seconds(10)), collector = ResponseCollector()
        let channel = try harness.connect(collector)
        try harness.send("upsert node N first", to: channel)
        XCTAssertEqual(harness.committed.wait(timeout: .now() + 2), .success)
        try harness.send("upsert node N second", to: channel)
        XCTAssertEqual(collector.received.wait(timeout: .now() + .milliseconds(200)), .timedOut)
        try harness.drain(); wait(for: [collector.closed], timeout: 2)
        XCTAssertTrue(collector.text.contains("200 OK"), collector.text)
        XCTAssertTrue(collector.text.contains("committedLSN"), collector.text)
        XCTAssertTrue(try harness.read("first").contains("\"id\":\"first\""))
        XCTAssertTrue(try harness.read("second").contains("notFound"))
    }
}
