// Presigned S3 URLs (apps/novaworld_web/content/s3_presign.h, ADR 0069 d6): AWS's own example of a
// presigned GET (the S3 guide's "Authenticating Requests: Using Query Parameters", examplebucket's
// test.txt on 2013-05-24 for a day) signed to its very signature; the session token carried and
// signed; a PUT that signs its checksum header names it in the signed headers and changes its
// signature with the header's value; and SigV4's percent-encoding of a key's path and a query
// value.

#include "content/s3_presign.h"

#include "common/test_expect.h"

#include <cstdio>
#include <string>

using namespace opennova::novaworld_server::content;

namespace {

PresignRequest example() {
	PresignRequest r;
	r.method = "GET";
	r.host = "examplebucket.s3.amazonaws.com";
	r.region = "us-east-1";
	r.key = "test.txt";
	r.expires_s = 86400;
	r.now_unix = 1369353600; // 2013-05-24T00:00:00Z
	r.credentials.key_id = "AKIAIOSFODNN7EXAMPLE";
	r.credentials.secret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
	return r;
}

std::string signature_of(const std::string &url) {
	const auto at = url.find("X-Amz-Signature=");
	return at == std::string::npos ? std::string() : url.substr(at + 16);
}

} // namespace

int main() {
	// AWS's example, to the byte.
	const std::string url = presign_s3_url(example());
	TEST_EXPECT(url ==
			"https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm=AWS4-HMAC-SHA256"
			"&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
			"&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host"
			"&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");

	// A task role's session token is in the query, under the signature.
	PresignRequest with_token = example();
	with_token.credentials.token = "token/with+chars";
	const std::string tokened = presign_s3_url(with_token);
	TEST_EXPECT(tokened.find("X-Amz-Security-Token=token%2Fwith%2Bchars") != std::string::npos);
	TEST_EXPECT(signature_of(tokened) != signature_of(url));

	// A PUT signs the checksum header it names: another value, another signature.
	PresignRequest put = example();
	put.method = "PUT";
	put.headers = { { "x-amz-checksum-sha256", "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=" } };
	const std::string signed_put = presign_s3_url(put);
	TEST_EXPECT(signed_put.find("X-Amz-SignedHeaders=host%3Bx-amz-checksum-sha256") !=
			std::string::npos);
	PresignRequest other = put;
	other.headers[0].second = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
	TEST_EXPECT(signature_of(presign_s3_url(other)) != signature_of(signed_put));
	TEST_EXPECT(signature_of(signed_put).size() == 64);

	// The encoding: a key's '/' kept, a query value's encoded; nothing unreserved touched.
	TEST_EXPECT(sigv4_encode("data/content/JO/blobs/ab", true) == "data/content/JO/blobs/ab");
	TEST_EXPECT(sigv4_encode("a/b c+d~e", false) == "a%2Fb%20c%2Bd~e");
	TEST_EXPECT(s3_host("bucket", "eu-west-2") == "bucket.s3.eu-west-2.amazonaws.com");
	std::printf("s3_presign: AWS's example to its signature, the token, a PUT's checksum signed\n");
	return 0;
}
