//===----------------------------------------------------------------------===//
// Decoding response bodies: every coding the encoder makes, the two shapes of
// deflate, and bodies that are cut short, corrupt or too large.
//===----------------------------------------------------------------------===//

import Testing
import CAvian
@testable import AvianCore
@testable import AvianHTTP

/// `input` encoded whole with the response encoder.
private func encode(_ input: [UInt8], _ coding: ContentCoding) -> [UInt8] {
    let encoder = av_enc_new(Int32(coding.rawValue))!
    defer { av_enc_free(encoder) }
    var output: [UInt8] = []
    var chunk = [UInt8](repeating: 0, count: 4096)
    var offset = 0
    while true {
        var consumed = 0
        var produced = 0
        let rc = input.withUnsafeBufferPointer { i in
            chunk.withUnsafeMutableBufferPointer { o in
                av_enc_run(encoder, i.baseAddress.map { $0 + offset }, i.count - offset, AV_ENC_FINISH,
                           o.baseAddress, o.count, &consumed, &produced)
            }
        }
        offset += consumed
        output += chunk[0..<produced]
        if rc == 0 { return output }
        precondition(rc == 1)
    }
}

private let text = Array(String(repeating: "garuda decodes what it asked for. ", count: 400).utf8)

@Suite struct ContentDecoderTests {
    @Test func everyCodingTheEncoderMakesRoundTrips() throws {
        for coding in [ContentCoding.gzip, .br, .zstd] where av_enc_available(Int32(coding.rawValue)) == 1 {
            let encoded = encode(text, coding)
            #expect(encoded.count < text.count)
            let name = "\(coding.token)"
            #expect(try ContentDecoder.decode(encoded, contentEncoding: name, limit: 1 << 20) == text)
            #expect(try ContentDecoder.decode(encoded, contentEncoding: name.uppercased(), limit: 1 << 20) == text)
        }
    }

    @Test func deflateIsZlibOrRaw() throws {
        let hello = Array("hello".utf8)
        let zlib: [UInt8] = [0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15]
        let raw: [UInt8] = [0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00]
        #expect(try ContentDecoder.decode(zlib, contentEncoding: "deflate", limit: 100) == hello)
        #expect(try ContentDecoder.decode(raw, contentEncoding: "deflate", limit: 100) == hello)
    }

    @Test func codingsAreUndoneLastFirst() throws {
        let twice = encode(encode(text, .gzip), .gzip)
        #expect(try ContentDecoder.decode(twice, contentEncoding: "gzip, gzip", limit: 1 << 20) == text)
        #expect(try ContentDecoder.decode(text, contentEncoding: "identity", limit: 1 << 20) == text)
        let once = encode(text, .gzip)
        #expect(try ContentDecoder.decode(once, contentEncoding: "identity, gzip", limit: 1 << 20) == text)
    }

    @Test func gzipMembersFollowOneAnother() throws {
        let first = Array("first ".utf8)
        let second = Array("second".utf8)
        let joined = encode(first, .gzip) + encode(second, .gzip)
        #expect(try ContentDecoder.decode(joined, contentEncoding: "x-gzip", limit: 100) == first + second)
    }

    @Test func aBodyThatInflatesPastTheLimitIsRefused() throws {
        let zeros = [UInt8](repeating: 0, count: 1 << 20)
        let bomb = encode(zeros, .gzip)
        #expect(bomb.count < 4096)
        #expect(throws: ContentDecoder.Failure.tooLarge) {
            try ContentDecoder.decode(bomb, contentEncoding: "gzip", limit: 64 * 1024)
        }
        #expect(try ContentDecoder.decode(bomb, contentEncoding: "gzip", limit: 1 << 20).count == 1 << 20)
    }

    @Test func cutShortCorruptOrTrailingBytesAreInvalid() {
        let encoded = encode(text, .gzip)
        #expect(throws: ContentDecoder.Failure.invalid) {
            try ContentDecoder.decode(Array(encoded.dropLast(10)), contentEncoding: "gzip", limit: 1 << 20)
        }
        var corrupt = encoded
        corrupt[20] ^= 0xff
        corrupt[21] ^= 0xff
        #expect(throws: ContentDecoder.Failure.invalid) {
            try ContentDecoder.decode(corrupt, contentEncoding: "gzip", limit: 1 << 20)
        }
        #expect(throws: ContentDecoder.Failure.invalid) {
            try ContentDecoder.decode(encoded + [1, 2, 3], contentEncoding: "gzip", limit: 1 << 20)
        }
        #expect(throws: ContentDecoder.Failure.invalid) {
            try ContentDecoder.decode([0x78], contentEncoding: "deflate", limit: 100)
        }
        #expect(throws: ContentDecoder.Failure.invalid) {
            try ContentDecoder.decode(Array("not gzip".utf8), contentEncoding: "gzip", limit: 100)
        }
    }

    @Test func anUnknownCodingIsUnsupported() {
        #expect(throws: ContentDecoder.Failure.unsupported("compress")) {
            try ContentDecoder.decode([1, 2], contentEncoding: "compress", limit: 100)
        }
        #expect(!ContentDecoder.canDecode("gzip, compress"))
        #expect(ContentDecoder.canDecode("gzip"))
        #expect(ContentDecoder.canDecode("identity"))
    }

    @Test func acceptEncodingNamesWhatCanBeDecoded() {
        let offered = ContentDecoder.acceptEncoding
        #expect(offered.hasSuffix("gzip, deflate"))
        #expect(offered.contains("br") == (av_dec_available(Int32(ContentCoding.br.rawValue)) == 1))
        #expect(offered.contains("zstd") == (av_dec_available(Int32(ContentCoding.zstd.rawValue)) == 1))
    }
}
