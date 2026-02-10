#include "protocol/kafka_codec.h"
#include <cassert>
#include <iostream>
using namespace strike::protocol;

int main() {
    std::cout << "═══════════════════════════════════════\n  StrikeMQ Kafka Codec Tests\n═══════════════════════════════════════\n\n";

    std::cout << "test_binary_reader_writer... ";
    { uint8_t buf[256]; BinaryWriter w(buf, 256);
      w.write_int8(42); w.write_int16(1234); w.write_int32(567890);
      w.write_int64(1234567890123LL); w.write_string("hello");
      BinaryReader r(buf, w.position());
      assert(r.read_int8() == 42); assert(r.read_int16() == 1234);
      assert(r.read_int32() == 567890); assert(r.read_int64() == 1234567890123LL);
      assert(r.read_string() == "hello"); }
    std::cout << "PASS\n";

    std::cout << "test_decode_header... ";
    { uint8_t buf[128]; BinaryWriter w(buf, 128);
      w.write_int16(0); w.write_int16(7); w.write_int32(42); w.write_string("test-client");
      BinaryReader r(buf, w.position());
      auto h = KafkaDecoder::decode_header(r);
      assert(h.api_key == ApiKey::Produce); assert(h.api_version == 7);
      assert(h.correlation_id == 42); assert(h.client_id == "test-client"); }
    std::cout << "PASS\n";

    std::cout << "test_api_versions_response... ";
    { uint8_t buf[512];
      size_t len = KafkaEncoder::encode_api_versions_response(buf, 512, 99, 0);
      assert(len > 0);
      BinaryReader r(buf, len);
      r.read_int32(); // size
      assert(r.read_int32() == 99); // correlation_id
      assert(r.read_int16() == 0);  // error code
    }
    std::cout << "PASS\n";

    std::cout << "\nAll tests passed! ✅\n"; return 0;
}
