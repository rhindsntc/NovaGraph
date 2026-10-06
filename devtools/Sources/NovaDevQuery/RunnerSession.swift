import Foundation
import GraphDBKit

struct RunnerResponse: Sendable {
    var status: Int = 200
    var contentType = "application/json; charset=utf-8"
    var body: Data

    static func failure(_ status: Int, _ code: String, _ message: String) -> RunnerResponse {
        struct Failure: Encodable {
            let schemaVersion = 2, ok = false
            let error: Detail
            struct Detail: Encodable { let code: String, message: String; let context: [String: String] = [:] }
        }
        return RunnerResponse(status: status, body: try! JSONEncoder().encode(Failure(error: .init(code: code, message: message))))
    }
    static func success(_ result: GraphQueryResult) throws -> RunnerResponse {
        struct Success: Encodable {
            let schemaVersion = 2, ok = true
            let data: GraphJSON, receipt: GraphMutationReceipt?
        }
        return RunnerResponse(body: try JSONEncoder().encode(Success(data: result.data, receipt: result.receipt)))
    }
    static func data(_ fields: [String: GraphJSON]) throws -> RunnerResponse {
        struct Success: Encodable { let schemaVersion = 2, ok = true; let data: [String: GraphJSON] }
        return RunnerResponse(body: try JSONEncoder().encode(Success(data: fields)))
    }
}

// Explicit tags avoid JSON number inference, and an array preserves names until
// duplicate detection (including canonically equivalent Unicode names) completes.
private struct QueryParameter: Decodable {
    let name: String
    let value: GraphValue
    enum CodingKeys: String, CodingKey { case name, type, value }
    init(from decoder: Decoder) throws {
        let fields = try decoder.container(keyedBy: CodingKeys.self)
        name = try fields.decode(String.self, forKey: .name)
        let type = try fields.decode(String.self, forKey: .type)
        func invalid() -> DecodingError {
            .dataCorruptedError(forKey: .value, in: fields, debugDescription: "Invalid typed parameter value.")
        }
        switch type {
        case "null": guard try fields.decodeNil(forKey: .value) else { throw invalid() }; value = .null
        case "bool": value = .bool(try fields.decode(Bool.self, forKey: .value))
        case "int": value = .int(try fields.decode(Int64.self, forKey: .value))
        case "double":
            let number = try fields.decode(Double.self, forKey: .value)
            guard number.isFinite else { throw invalid() }; value = .double(number)
        case "string": value = .string(try fields.decode(String.self, forKey: .value))
        default: throw invalid()
        }
    }
}

// Database state is confined to the single worker; admission counters have their own lock.
final class RunnerSession: @unchecked Sendable {
    let origin: String, token = UUID().uuidString + UUID().uuidString
    let persistent: Bool, webRoot: URL
    private(set) var databasePath: URL
    private var db: GraphDatabase
    private let lock = NSLock()
    private var busy = false, connections = 0
    private var temporaryPaths: [URL] = []

    init(port: Int, webRoot: URL, database: URL?) throws {
        origin = "http://127.0.0.1:\(port)"
        self.webRoot = webRoot.resolvingSymlinksInPath()
        guard FileManager.default.fileExists(atPath: webRoot.appendingPathComponent("index.html").path) else {
            throw RunnerStartupError.message("Missing built docs app; run make docs-build, or pass --web-root to its dist directory.")
        }
        persistent = database != nil
        databasePath = database ?? Self.temporaryPath()
        db = try Self.open(databasePath)
        if !persistent { temporaryPaths.append(databasePath) }
    }
    private static func temporaryPath() -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("nova-runner-\(UUID().uuidString)", isDirectory: true)
    }
    private static func open(_ path: URL) throws -> GraphDatabase {
        var options = GraphQueryOptions()
        options.resultBytes = 1024 * 1024
        options.maxResults = 1000
        return try GraphDatabase(configuration: .init(path: path, queryOptions: options))
    }
    func admitConnection() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard connections < 16 else { return false }; connections += 1; return true
    }
    func releaseConnection() { lock.lock(); connections -= 1; lock.unlock() }
    func beginOperation() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard !busy else { return false }; busy = true; return true
    }
    func endOperation() { lock.lock(); busy = false; lock.unlock() }
    func close() throws {
        try db.close()
        for path in temporaryPaths { try FileManager.default.removeItem(at: path) }
        temporaryPaths.removeAll()
    }
    func execute(path: String, body: Data, request: GraphRequest) -> RunnerResponse {
        do {
            switch path {
            case "/dev/session":
                return try RunnerResponse.data(["mode":.string(persistent ? "persistent" : "disposable"),
                    "databasePath":.string(databasePath.path)])
            case "/dev/reset":
                guard !persistent else { return .failure(409, "conflict", "Reset is disabled for explicitly persistent databases.") }
                let replacementPath = Self.temporaryPath()
                let replacement = try Self.open(replacementPath)
                temporaryPaths.append(replacementPath)
                let oldPath = databasePath
                try db.close()
                db = replacement; databasePath = replacementPath
                try FileManager.default.removeItem(at: oldPath)
                temporaryPaths.removeAll { $0 == oldPath }
                return try RunnerResponse.data(["reset":.bool(true), "mode":.string("disposable"), "databasePath":.string(databasePath.path)])
            case "/dev/query":
                struct Query: Decodable { let query: String; let parameters: [QueryParameter]? }
                let input = try JSONDecoder().decode(Query.self, from: body)
                var parameters: [String: GraphValue] = [:]
                for parameter in input.parameters ?? [] {
                    guard parameters.updateValue(parameter.value, forKey: parameter.name) == nil else {
                        return .failure(400, "invalidArgument", "Duplicate parameter name.")
                    }
                }
                return try .success(db.queryResult(input.query, parameters: parameters, request: request))
            case "/dev/inspect/nodes", "/dev/inspect/edges", "/dev/inspect/indexes":
                struct Page: Decodable { let cursor: String?; let limit: UInt32? }
                let input = try JSONDecoder().decode(Page.self, from: body)
                let kind: GraphInspectionKind = path.hasSuffix("nodes") ? .nodes : path.hasSuffix("edges") ? .edges : .indexes
                return try .success(db.inspect(kind, cursor: input.cursor, limit: input.limit ?? 50, request: request))
            default: return .failure(404, "notFound", "Unknown runner endpoint.")
            }
        } catch let error as GraphCoreError {
            struct Failure: Encodable { let schemaVersion = 2, ok = false; let error: GraphCoreError }
            let status = error.code == "invalidArgument" ? 400 : error.code == "conflict" ? 409 : 422
            return RunnerResponse(status: status, body: (try? JSONEncoder().encode(Failure(error: error))) ?? Data())
        } catch is DecodingError { return .failure(400, "invalidArgument", "Expected query with an optional parameters array of {name,type,value}, or valid cursor/limit fields.") }
          catch { return .failure(500, "ioError", String(describing: error)) }
    }
    func asset(_ path: String) -> RunnerResponse {
        // Exact built assets only. No percent-decoding, SPA fallback, directory traversal or symlink escape.
        guard path == "/" || path == "/index.html" || path.hasPrefix("/assets/") || path == "/favicon.svg",
              !path.contains("%"), !path.contains(".."), !path.contains("\\"), !path.contains("?"), path.utf8.count < 512 else {
            return .failure(404, "notFound", "Asset not found.")
        }
        let relative = path == "/" ? "index.html" : String(path.dropFirst())
        let file = webRoot.appendingPathComponent(relative).resolvingSymlinksInPath()
        guard file.path.hasPrefix(webRoot.path + "/"),
              let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize, size <= 4*1024*1024,
              var data = try? Data(contentsOf: file) else { return .failure(404, "notFound", "Asset not found.") }
        let types = ["html":"text/html; charset=utf-8", "js":"text/javascript; charset=utf-8", "css":"text/css; charset=utf-8", "svg":"image/svg+xml"]
        guard let contentType = types[file.pathExtension] else { return .failure(404, "notFound", "Asset not found.") }
        if file.pathExtension == "html" {
            var html = String(decoding: data, as: UTF8.self)
            html = html.replacingOccurrences(of: "</head>", with: "<meta name=\"nova-session\" content=\"\(token)\"><meta name=\"nova-mode\" content=\"\(persistent ? "persistent" : "disposable")\"></head>")
            data = Data(html.utf8)
        }
        return RunnerResponse(contentType: contentType, body: data)
    }
}

enum RunnerStartupError: Error { case message(String) }
