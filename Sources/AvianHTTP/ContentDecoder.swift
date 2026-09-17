//===----------------------------------------------------------------------===//
// Undoing a response's content coding, for a client.
//
// A client says which codings it can read in Accept-Encoding, and the server
// names the ones it applied in Content-Encoding, in the order it applied them.
// This decodes them in reverse, with the decoded size held to a limit as it
// grows: a few kilobytes of gzip can inflate to gigabytes, and a limit checked
// only at the end would already have spent the memory.
//===----------------------------------------------------------------------===//

import CAvian
import AvianCore

public enum ContentDecoder {
    /// Why a body could not be decoded.
    public enum Failure: Error, Equatable, Sendable {
        /// A coding this process has no decoder for.
        case unsupported(String)
        /// Bytes that are not what the coding produces, or a stream that
        /// stops before its end.
        case invalid
        /// The decoded body would be larger than the limit.
        case tooLarge
    }

    /// The codings this process can decode, for an Accept-Encoding header:
    /// gzip and deflate always, brotli and zstd when their libraries load.
    public static var acceptEncoding: String {
        var codings: [String] = []
        if av_dec_available(Int32(ContentCoding.zstd.rawValue)) == 1 { codings.append("zstd") }
        if av_dec_available(Int32(ContentCoding.br.rawValue)) == 1 { codings.append("br") }
        codings.append("gzip")
        codings.append("deflate")
        return codings.joined(separator: ", ")
    }

    /// Whether `contentEncoding` names only codings this process can decode.
    public static func canDecode(_ contentEncoding: String) -> Bool {
        codings(contentEncoding).allSatisfy { codec($0) != nil }
    }

    /// `body` with the codings `contentEncoding` names undone, last applied
    /// first. `identity` is no coding. At most `limit` bytes are produced.
    public static func decode(_ body: [UInt8], contentEncoding: String,
                              limit: Int) throws(Failure) -> [UInt8] {
        var bytes = body
        for coding in codings(contentEncoding).reversed() {
            guard let codec = codec(coding) else { throw .unsupported(coding) }
            bytes = try decode(bytes, codec: codec, limit: limit)
        }
        return bytes
    }

    /// The codings named, lower-cased, without `identity`.
    static func codings(_ header: String) -> [String] {
        header.split(separator: ",").compactMap { part in
            let coding = part.trimmingSpaces.lowercased()
            return coding.isEmpty || coding == "identity" ? nil : coding
        }
    }

    static func codec(_ coding: String) -> Int32? {
        let number: Int32
        switch coding {
        case "gzip", "x-gzip": number = Int32(ContentCoding.gzip.rawValue)
        case "deflate": number = AV_DEC_DEFLATE
        case "br": number = Int32(ContentCoding.br.rawValue)
        case "zstd": number = Int32(ContentCoding.zstd.rawValue)
        default: return nil
        }
        return av_dec_available(number) == 1 ? number : nil
    }

    static func decode(_ input: [UInt8], codec: Int32, limit: Int) throws(Failure) -> [UInt8] {
        guard let decoder = av_dec_new(codec) else { throw .unsupported("\(codec)") }
        defer { av_dec_free(decoder) }
        var output: [UInt8] = []
        output.reserveCapacity(min(limit, max(1024, input.count * 4)))
        var chunk = [UInt8](repeating: 0, count: 16 * 1024)
        var offset = 0
        while true {
            var consumed = 0
            var produced = 0
            let rc = input.withUnsafeBufferPointer { inBuffer in
                chunk.withUnsafeMutableBufferPointer { out in
                    av_dec_run(decoder, inBuffer.baseAddress.map { $0 + offset }, inBuffer.count - offset,
                               out.baseAddress, out.count, &consumed, &produced)
                }
            }
            offset += consumed
            if produced > 0 {
                guard output.count + produced <= limit else { throw .tooLarge }
                output.append(contentsOf: chunk[0..<produced])
            }
            switch rc {
            case 2:
                // Bytes after the end are not part of any coding the server
                // named.
                guard offset == input.count else { throw .invalid }
                return output
            case 1:
                continue
            case 0:
                // No progress with the stream not over: it was cut short.
                if consumed == 0 && produced == 0 { throw .invalid }
            default:
                throw .invalid
            }
        }
    }
}

extension Substring {
    fileprivate var trimmingSpaces: Substring {
        var s = self
        while let first = s.first, first == " " || first == "\t" { s.removeFirst() }
        while let last = s.last, last == " " || last == "\t" { s.removeLast() }
        return s
    }
}
