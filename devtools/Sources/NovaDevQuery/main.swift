import Foundation
import Dispatch
import NIO
import NIOHTTP1
import NIOPosix

func run() throws {
    var port = 8080, database: URL?
    var webRoot = URL(fileURLWithPath: "docs-web/dist", isDirectory: true)
    var args = Array(CommandLine.arguments.dropFirst())
    while !args.isEmpty {
        let flag = args.removeFirst()
        if flag == "--help" {
            print("NovaDevQuery [--port 8080] [--web-root docs-web/dist] [--database /explicit/persistent/path]\nBinds 127.0.0.1 only. Default database is disposable; reset never deletes a persistent database.")
            return
        }
        guard !args.isEmpty else { throw RunnerStartupError.message("Missing value for \(flag)") }
        let value = args.removeFirst()
        switch flag {
        case "--port": guard let number = Int(value), (1...65535).contains(number) else { throw RunnerStartupError.message("Invalid port") }; port = number
        case "--web-root": webRoot = URL(fileURLWithPath: value, isDirectory: true)
        case "--database": database = URL(fileURLWithPath: value, isDirectory: true)
        default: throw RunnerStartupError.message("Unknown option \(flag)")
        }
    }
    // Old ambient database/host overrides could silently expose or reopen user data.
    guard ProcessInfo.processInfo.environment["NOVA_DB_PATH"] == nil,
          ProcessInfo.processInfo.environment["NOVA_DEV_HOST"] == nil else {
        throw RunnerStartupError.message("Remove NOVA_DB_PATH/NOVA_DEV_HOST; use explicit --database. Binding is always loopback.")
    }
    let session = try RunnerSession(port: port, webRoot: webRoot, database: database)
    let group = MultiThreadedEventLoopGroup(numberOfThreads: 1)
    let pool = NIOThreadPool(numberOfThreads: 1); pool.start()
    defer { try? pool.syncShutdownGracefully(); try? session.close(); try? group.syncShutdownGracefully() }
    var limits = NIOHTTPDecoderLimitConfiguration()
    limits.maxHeaderFieldSize = 8192; limits.maxHeaderListSize = 16384; limits.maxHeaderFieldCount = 32
    let decoderLimits = limits
    let channel = try ServerBootstrap(group: group)
        .serverChannelOption(ChannelOptions.backlog, value: 16)
        .serverChannelOption(ChannelOptions.socketOption(.so_reuseaddr), value: 1)
        .childChannelInitializer { channel in
            guard session.admitConnection() else { return channel.close() }
            return channel.pipeline.configureHTTPServerPipeline(withPipeliningAssistance: false, withDecoderLimitConfiguration: decoderLimits).flatMap {
                channel.pipeline.addHandler(DevQueryHandler(session: session, pool: pool))
            }.flatMapError { error in session.releaseConnection(); return channel.eventLoop.makeFailedFuture(error) }
        }
        .childChannelOption(ChannelOptions.maxMessagesPerRead, value: 4)
        .childChannelOption(ChannelOptions.recvAllocator, value: FixedSizeRecvByteBufferAllocator(capacity: 8192))
        .bind(host: "127.0.0.1", port: port).wait()
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN)
    let signals = [SIGINT, SIGTERM].map { number in
        let source = DispatchSource.makeSignalSource(signal: number, queue: .global())
        source.setEventHandler { channel.close(promise: nil) }; source.resume(); return source
    }
    defer { signals.forEach { $0.cancel() } }
    print("Nova local runner: \(session.origin)/#playground\nDatabase: \(session.databasePath.path) (\(session.persistent ? "persistent" : "disposable"))")
    try channel.closeFuture.wait()
    try pool.syncShutdownGracefully()
    try session.close()
}
do { try run() }
catch { FileHandle.standardError.write(Data("NovaDevQuery: \(error)\n".utf8)); exit(1) }
