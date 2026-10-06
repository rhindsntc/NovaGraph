import Foundation

// Captures the actual Encodable scalar methods, before JSON can erase an integral
// Double's type. A property may use a custom single-value Encodable representation;
// nested keyed/unkeyed containers are deliberately unsupported.
private final class PropertyState {
    var values: [String: GraphValue] = [:]
    var invalid = false
    var keyedRoot = false
}
private func checkedInteger<T: BinaryInteger>(_ value: T) throws -> Int64 {
    guard let result = Int64(exactly: value) else { throw GraphDBError.invalidPropertyPayload }
    return result
}
func encodedProperties<T: Encodable>(_ model: T) throws -> [String: GraphValue] {
    let state = PropertyState()
    try model.encode(to: PropertyEncoder(state: state))
    guard state.keyedRoot, !state.invalid else { throw GraphDBError.invalidPropertyPayload }
    return state.values
}
private struct PropertyEncoder: Encoder {
    let state: PropertyState
    var key: String? = nil
    var codingPath: [any CodingKey] { [] }
    var userInfo: [CodingUserInfoKey: Any] { [:] }
    func store(_ value: GraphValue) throws {
        guard let key else { throw GraphDBError.invalidPropertyPayload }
        state.values[key] = try value.validated()
    }
    func container<K: CodingKey>(keyedBy type: K.Type) -> KeyedEncodingContainer<K> {
        if key != nil { state.invalid = true } else { state.keyedRoot = true }
        return KeyedEncodingContainer(Properties<K>(encoder: self))
    }
    func unkeyedContainer() -> any UnkeyedEncodingContainer { state.invalid = true; return RejectedArray(encoder: self) }
    func singleValueContainer() -> any SingleValueEncodingContainer { Scalar(encoder: self) }
}
private struct Properties<K: CodingKey>: KeyedEncodingContainerProtocol {
    typealias Key = K
    let encoder: PropertyEncoder
    var codingPath: [any CodingKey] { [] }
    func scalar(_ key: K) -> PropertyEncoder { PropertyEncoder(state: encoder.state, key: key.stringValue) }
    mutating func encodeNil(forKey key: K) throws { try scalar(key).store(.null) }
    mutating func encode(_ value: Bool, forKey key: K) throws { try scalar(key).store(.bool(value)) }
    mutating func encode(_ value: String, forKey key: K) throws { try scalar(key).store(.string(value)) }
    mutating func encode(_ value: Double, forKey key: K) throws { try scalar(key).store(.double(Double(value))) }
    mutating func encode(_ value: Float, forKey key: K) throws { try scalar(key).store(.double(Double(value))) }
    mutating func encode(_ value: Int, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int8, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int16, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int32, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int64, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt8, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt16, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt32, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt64, forKey key: K) throws { try scalar(key).store(.int(try checkedInteger(value))) }
    mutating func encode<T: Encodable>(_ value: T, forKey key: K) throws { try value.encode(to: scalar(key)) }
    mutating func nestedContainer<N: CodingKey>(keyedBy type: N.Type, forKey key: K) -> KeyedEncodingContainer<N> {
        encoder.state.invalid = true; return scalar(key).container(keyedBy: type)
    }
    mutating func nestedUnkeyedContainer(forKey key: K) -> any UnkeyedEncodingContainer { scalar(key).unkeyedContainer() }
    mutating func superEncoder() -> any Encoder { encoder.state.invalid = true; return encoder }
    mutating func superEncoder(forKey key: K) -> any Encoder { encoder.state.invalid = true; return scalar(key) }
}
private struct Scalar: SingleValueEncodingContainer {
    let encoder: PropertyEncoder
    var codingPath: [any CodingKey] { [] }
    mutating func encodeNil() throws { try encoder.store(.null) }
    mutating func encode(_ value: Bool) throws { try encoder.store(.bool(value)) }
    mutating func encode(_ value: String) throws { try encoder.store(.string(value)) }
    mutating func encode(_ value: Double) throws { try encoder.store(.double(Double(value))) }
    mutating func encode(_ value: Float) throws { try encoder.store(.double(Double(value))) }
    mutating func encode(_ value: Int) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int8) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int16) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int32) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: Int64) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt8) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt16) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt32) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode(_ value: UInt64) throws { try encoder.store(.int(try checkedInteger(value))) }
    mutating func encode<T: Encodable>(_ value: T) throws { try value.encode(to: encoder) }
}
private struct RejectedArray: UnkeyedEncodingContainer {
    let encoder: PropertyEncoder
    var codingPath: [any CodingKey] { [] }
    var count: Int { 0 }
    mutating func encodeNil() throws { throw GraphDBError.invalidPropertyPayload }
    mutating func encode<T: Encodable>(_ value: T) throws { throw GraphDBError.invalidPropertyPayload }
    mutating func nestedContainer<K: CodingKey>(keyedBy type: K.Type) -> KeyedEncodingContainer<K> { encoder.container(keyedBy: type) }
    mutating func nestedUnkeyedContainer() -> any UnkeyedEncodingContainer { self }
    mutating func superEncoder() -> any Encoder { encoder }
}
