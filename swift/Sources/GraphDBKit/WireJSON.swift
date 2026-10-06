import Foundation

/// Keeps object members and scalar lexemes intact until a caller chooses a Swift
/// dictionary. Native identifiers are byte-distinct; Swift String keys are not.
private indirect enum WireJSON {
    case scalar(Range<Int>)
    case array([WireJSON])
    case object([(key: String, spelling: Range<Int>, value: WireJSON)])

    func validateDictionaryKeys() throws {
        switch self {
        case .object(let fields):
            var keys = Set<String>()
            for field in fields {
                guard keys.insert(field.key).inserted else {
                    throw GraphDBError.decodingFailed("Duplicate or Unicode-equivalent keys cannot be represented by a Swift dictionary; use rawQuery")
                }
                try field.value.validateDictionaryKeys()
            }
        case .array(let elements): try elements.forEach { try $0.validateDictionaryKeys() }
        case .scalar: break
        }
    }

    func appendUntagged(to output: inout [UInt8], bytes: [UInt8], allowScalar: Bool = false) {
        switch self {
        case .scalar(let range): output.append(contentsOf: bytes[range])
        case .array(let elements):
            output.append(91)
            for (index, value) in elements.enumerated() {
                if index > 0 { output.append(44) }
                value.appendUntagged(to: &output, bytes: bytes)
            }
            output.append(93)
        case .object(let fields):
            // Root/array records and projections are never scalar wrappers.
            if allowScalar, fields.count == 2,
               let type = fields.first(where: { $0.key == "type" }),
               let value = fields.first(where: { $0.key == "value" }),
               case .scalar(let tag) = type.value, case .scalar = value.value,
               ["\"null\"", "\"bool\"", "\"int\"", "\"double\"", "\"string\""].contains(String(decoding: bytes[tag], as: UTF8.self)) {
                value.value.appendUntagged(to: &output, bytes: bytes)
                return
            }
            output.append(123)
            for (index, field) in fields.enumerated() {
                if index > 0 { output.append(44) }
                output.append(contentsOf: bytes[field.spelling]); output.append(58)
                field.value.appendUntagged(to: &output, bytes: bytes, allowScalar: true)
            }
            output.append(125)
        }
    }
}

/// JSONDecoder validates syntax first. This scanner retains byte ranges rather
/// than normalizing object keys or converting numbers through NSNumber.
private struct WireScanner {
    let bytes: [UInt8]
    let request: GraphRequest?
    var index = 0
    mutating func space() { while index < bytes.count && [9, 10, 13, 32].contains(bytes[index]) { index += 1 } }
    mutating func take(_ byte: UInt8) throws {
        space()
        guard index < bytes.count, bytes[index] == byte else { throw GraphDBError.decodingFailed("Invalid wire JSON") }
        index += 1
    }
    mutating func string() throws -> Range<Int> {
        space(); let start = index
        try take(34)
        while index < bytes.count {
            if index & 255 == 0 { try request?.check() }
            let byte = bytes[index]; index += 1
            if byte == 34 { return start..<index }
            if byte == 92 { index += 1 }
        }
        throw GraphDBError.decodingFailed("Unterminated wire JSON string")
    }
    mutating func value(depth: Int = 0) throws -> WireJSON {
        try request?.check()
        space()
        guard depth < 128, index < bytes.count else { throw GraphDBError.decodingFailed("Invalid wire JSON depth") }
        switch bytes[index] {
        case 123:
            index += 1; space()
            var fields: [(key: String, spelling: Range<Int>, value: WireJSON)] = []
            if index < bytes.count && bytes[index] == 125 { index += 1; return .object(fields) }
            while true {
                let spelling = try string()
                let key = try JSONDecoder().decode(String.self, from: Data(bytes[spelling]))
                try take(58)
                fields.append((key, spelling, try value(depth: depth + 1)))
                space()
                if index < bytes.count && bytes[index] == 125 { index += 1; return .object(fields) }
                try take(44)
            }
        case 91:
            index += 1; space(); var values: [WireJSON] = []
            if index < bytes.count && bytes[index] == 93 { index += 1; return .array(values) }
            while true {
                values.append(try value(depth: depth + 1)); space()
                if index < bytes.count && bytes[index] == 93 { index += 1; return .array(values) }
                try take(44)
            }
        case 34: return .scalar(try string())
        default:
            let start = index
            while index < bytes.count && ![9, 10, 13, 32, 44, 93, 125].contains(bytes[index]) { index += 1 }
            guard index > start else { throw GraphDBError.decodingFailed("Invalid wire JSON scalar") }
            return .scalar(start..<index)
        }
    }
}

func validateWireDictionaryKeys(_ data: Data, request: GraphRequest? = nil) throws {
    var scanner = WireScanner(bytes: Array(data), request: request)
    try scanner.value().validateDictionaryKeys()
}

func decodeRawV2(_ data: Data, request: GraphRequest? = nil) throws -> String {
    struct Envelope: Decodable { let schemaVersion: Int; let ok: Bool; let error: GraphCoreError?; let receipt: GraphMutationReceipt? }
    let envelope: Envelope
    do { envelope = try JSONDecoder().decode(Envelope.self, from: data) }
    catch { throw GraphDBError.decodingFailed(String(describing: error)) }
    guard envelope.schemaVersion == 2 else { throw GraphDBError.decodingFailed("Unsupported response schema version") }
    if !envelope.ok {
        guard let error = envelope.error else { throw GraphDBError.decodingFailed("Missing error status") }
        throw error
    }
    let reader = envelope.receipt == nil ? request : nil
    try reader?.check()
    var scanner = WireScanner(bytes: Array(data), request: reader)
    guard case .object(let fields) = try scanner.value(), let payload = fields.first(where: { $0.key == "data" }) else {
        throw GraphDBError.decodingFailed("Missing result data")
    }
    var output: [UInt8] = []; output.reserveCapacity(data.count)
    payload.value.appendUntagged(to: &output, bytes: scanner.bytes)
    try reader?.check()
    return String(decoding: output, as: UTF8.self)
}
