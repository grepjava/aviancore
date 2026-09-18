import CAvian
import Testing

// The rule by which a certificate is matched to the name a client asked for.
// Both certificate paths use it: OpenSSL's SNI callback for TCP, and the QUIC
// handshake's own selection, which has no OpenSSL handshake to do it.
//
// Matching a whole certificate -- walking its subject alternative names, or
// its common name when it has none -- is `av_certkey_matches`, and it is
// proved end to end by Garuda's scripts/sni-test.sh, which serves real
// certificates over both transports. What is here is the rule itself, where
// every mistake anyone makes about wildcards lives.

private func matches(_ pattern: String, _ host: String) -> Bool {
    pattern.withCString { p in host.withCString { h in av_host_matches(p, h) != 0 } }
}

@Suite("Host matching")
struct HostMatchTests {
    @Test func aNameMatchesItself() {
        #expect(matches("one.example", "one.example"))
        #expect(!matches("one.example", "two.example"))
    }

    @Test func caseIsNotPartOfAName() {
        #expect(matches("One.Example", "one.EXAMPLE"))
    }

    /// RFC 6125: a wildcard covers exactly one label, and not the empty one.
    @Test func aWildcardCoversOneLabel() {
        #expect(matches("*.example.com", "a.example.com"))
        #expect(!matches("*.example.com", "a.b.example.com"))
        #expect(!matches("*.example.com", "example.com"))
    }

    /// The certificate that claims `*.example.com` does not claim
    /// `example.com`, which is the mistake that serves the wrong certificate
    /// for the bare domain.
    @Test func aWildcardDoesNotCoverTheDomainItself() {
        #expect(!matches("*.example.com", "example.com"))
        #expect(matches("example.com", "example.com"))
    }

    /// A wildcard anywhere but leftmost is not a wildcard, and is matched
    /// literally -- which nothing will ever be called.
    @Test func aWildcardInTheMiddleIsNotOne() {
        #expect(!matches("a.*.example.com", "a.b.example.com"))
        #expect(!matches("one*.example", "oneplus.example"))
    }

    @Test func nothingMatchesNothing() {
        #expect(!matches("", "one.example"))
        #expect(!matches("one.example", ""))
        #expect(!matches("*.", "a."))
    }
}
