import CGraphDB

/// Query safety ceilings can be lowered per database. Defaults come from the native engine.
public struct GraphQueryOptions: Sendable {
    public var maxDepth: Int
    public var maxResults: Int
    public var maxExpandedEdges: Int
    public var workingBytes: Int
    public var resultBytes: Int
    public var maxStatements: Int
    public var batchBytes: Int
    public var timeoutMilliseconds: Int

    public init() {
        let defaults=graphdb_default_query_options()
        maxDepth=Int(defaults.max_depth)
        maxResults=Int(defaults.max_results)
        maxExpandedEdges=Int(defaults.max_expanded_edges)
        workingBytes=Int(defaults.working_bytes)
        resultBytes=Int(defaults.result_bytes)
        maxStatements=Int(defaults.max_statements)
        batchBytes=Int(defaults.batch_bytes)
        timeoutMilliseconds=Int(defaults.timeout_ms)
    }
    func native() throws -> GraphDBQueryOptions {
        let defaults=GraphQueryOptions()
        guard (0...defaults.maxDepth).contains(maxDepth),
              (0...defaults.maxResults).contains(maxResults),
              (0...defaults.maxExpandedEdges).contains(maxExpandedEdges),
              (1...defaults.workingBytes).contains(workingBytes),
              (1...defaults.resultBytes).contains(resultBytes),
              (1...defaults.maxStatements).contains(maxStatements),
              (1...defaults.batchBytes).contains(batchBytes),
              (1...defaults.timeoutMilliseconds).contains(timeoutMilliseconds) else {
            throw GraphDBError.queryFailed("invalidArgument: query options exceed supported bounds")
        }
        return GraphDBQueryOptions(max_depth: UInt64(maxDepth), max_results: UInt64(maxResults),
            max_expanded_edges: UInt64(maxExpandedEdges), working_bytes: UInt64(workingBytes),
            result_bytes: UInt64(resultBytes), max_statements: UInt64(maxStatements),
            batch_bytes: UInt64(batchBytes), timeout_ms: UInt64(timeoutMilliseconds))
    }
}
