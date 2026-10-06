import Foundation
import GraphDBKit
import NIO
import NIOHTTP1
import NIOPosix

// All handler fields are confined to its channel's event loop.
final class DevQueryHandler: ChannelInboundHandler, @unchecked Sendable {
    typealias InboundIn = HTTPServerRequestPart
    typealias OutboundOut = HTTPServerResponsePart
    private let session: RunnerSession, pool: NIOThreadPool
    private let operation: @Sendable (String, Data, GraphRequest) -> RunnerResponse
    private let requestTimeout: TimeAmount
    private var head: HTTPRequestHead?
    private var body = ByteBuffer()
    private var finished = false, executing = false
    private var timer: Scheduled<Void>?
    private var request: GraphRequest?

    init(session: RunnerSession, pool: NIOThreadPool, requestTimeout: TimeAmount = .seconds(10),
         operation: (@Sendable (String, Data, GraphRequest) -> RunnerResponse)? = nil) {
        self.session = session; self.pool = pool; self.requestTimeout = requestTimeout
        self.operation = operation ?? { session.execute(path: $0, body: $1, request: $2) }
    }
    func handlerAdded(context: ChannelHandlerContext) {
        let bound = NIOLoopBound(context, eventLoop: context.eventLoop)
        timer = context.eventLoop.scheduleTask(in: requestTimeout) {
            self.request?.cancel()
            let response = self.executing
                ? RunnerResponse.failure(408, "outcomeUnknown", "Request deadline expired after admission; the operation may have committed. Inspect database state before retrying.")
                : RunnerResponse.failure(408, "timeout", "Request deadline expired before admission.")
            self.write(response, context: bound.value)
        }
    }
    func channelInactive(context: ChannelHandlerContext) {
        finished = true; timer?.cancel(); request?.cancel(); session.releaseConnection()
        context.fireChannelInactive()
    }
    func errorCaught(context: ChannelHandlerContext, error: Error) { context.close(promise: nil) }
    func channelRead(context: ChannelHandlerContext, data: NIOAny) {
        // The admitted request owns the response; pipelined input cannot replace its receipt.
        guard !finished, !executing else { return }
        switch unwrapInboundIn(data) {
        case .head(let incoming):
            guard head == nil else { write(.failure(400, "invalidArgument", "One request per connection."), context: context); return }
            head = incoming
            guard incoming.headers["host"] == [String(session.origin.dropFirst("http://".count))] else {
                write(.failure(403, "forbidden", "Unexpected Host."), context: context); return
            }
            if incoming.uri.hasPrefix("/dev/") {
                guard incoming.method == .POST else { write(.failure(405, "invalidArgument", "Use POST."), context: context); return }
                guard incoming.headers["origin"] == [session.origin], incoming.headers["x-nova-session"] == [session.token] else {
                    write(.failure(403, "forbidden", "Expected same-origin session credentials."), context: context); return
                }
                guard incoming.headers["content-type"].count == 1,
                      incoming.headers["content-type"].first?.split(separator: ";", omittingEmptySubsequences: false).first?.trimmingCharacters(in: .whitespaces).lowercased() == "application/json" else {
                    write(.failure(415, "invalidArgument", "Use application/json."), context: context); return
                }
            } else if incoming.method != .GET {
                write(.failure(405, "invalidArgument", "Use GET for assets."), context: context); return
            }
            if let length = incoming.headers["content-length"].first, Int(length).map({ $0 > 65536 }) ?? true {
                write(.failure(413, "limitExceeded", "Request body exceeds 64 KiB."), context: context)
            }
        case .body(var part):
            guard part.readableBytes <= 65536 - body.readableBytes else {
                write(.failure(413, "limitExceeded", "Request body exceeds 64 KiB."), context: context); return
            }
            body.writeBuffer(&part)
        case .end:
            guard let head else { return }
            let engineOperation = head.method != .GET
            guard !engineOperation || session.beginOperation() else { write(.failure(503, "unavailable", "Runner busy; retry when the active operation finishes."), context: context); return }
            let data = Data(body.readableBytesView)
            let bound = NIOLoopBound(context, eventLoop: context.eventLoop)
            do { request = try GraphRequest(timeoutMilliseconds: 5000) }
            catch { if engineOperation { session.endOperation() }; write(.failure(500, "ioError", String(describing: error)), context: context); return }
            let request = request!
            executing = true
            pool.runIfActive(eventLoop: context.eventLoop) { [session, operation] in
                head.method == .GET ? session.asset(head.uri) : operation(head.uri, data, request)
            }.whenComplete { result in
                if engineOperation { self.session.endOperation() }
                switch result {
                case .success(let response): self.write(response, context: bound.value)
                case .failure(let error): self.write(.failure(503, "unavailable", String(describing: error)), context: bound.value)
                }
            }
        }
    }
    private func write(_ response: RunnerResponse, context: ChannelHandlerContext) {
        guard !finished else { return }; finished = true; timer?.cancel()
        let headers = HTTPHeaders([
            ("content-type", response.contentType), ("content-length", "\(response.body.count)"),
            ("connection", "close"), ("cache-control", "no-store"), ("x-content-type-options", "nosniff"),
            ("referrer-policy", "no-referrer"), ("cross-origin-resource-policy", "same-origin"),
            ("content-security-policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
        ])
        context.write(wrapOutboundOut(.head(.init(version: .http1_1, status: .init(statusCode: response.status), headers: headers))), promise: nil)
        var buffer = context.channel.allocator.buffer(capacity: response.body.count); buffer.writeBytes(response.body)
        context.write(wrapOutboundOut(.body(.byteBuffer(buffer))), promise: nil)
        context.writeAndFlush(wrapOutboundOut(.end(nil))).whenComplete { _ in context.close(promise: nil) }
    }
}
