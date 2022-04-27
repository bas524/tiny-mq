//
// Created by Alexander Bychuk on 12.04.2022.
//

#include "StreamMessageTest.h"
#include "StreamMessage.h"

using tiny_mq::StreamMessage;
using namespace tiny_mq::property::raw_type;

TEST_F(StreamMessageTest, testClearBody) {
  StreamMessage msg;

  for (int i = 0; i < 10; ++i) {
    EXPECT_NO_THROW(msg.write(i));
  }

  EXPECT_FALSE(msg.empty());

  msg.clearData();

  EXPECT_TRUE(msg.empty());
}

TEST_F(StreamMessageTest, testToJson) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write(42));
  EXPECT_NO_THROW(msg.write(std::string("42")));
  EXPECT_NO_THROW(msg.write(42.42f));

  EXPECT_FALSE(msg.empty());
  EXPECT_NO_THROW(msg.setProperty("prop-int", tiny_mq::property::Int(22)));
  EXPECT_NO_THROW(msg.setProperty("prop-null-bytes", tiny_mq::property::Bytes()));

  Poco::JSON::Object object;
  EXPECT_NO_THROW(object = msg.toJSON());
  std::stringstream ss;
  EXPECT_NO_THROW(object.stringify(ss, 1));
  std::string json = ss.str();
  EXPECT_EQ(json,
            "{\n \"data\": {\n  \"stream\": [\n   {\n    \"type\": \"INTEGER\",\n    \"value\": 42\n   },\n   {\n    \"type\": \"STRING\",\n    "
            "\"value\": \"42\"\n   },\n   {\n    \"type\": \"FLOAT\",\n    \"value\": 42.42\n   }\n  ]\n },\n \"number\": 0,\n \"persistentInfo\": "
            "{\n  \"fileFromName\": \"\",\n  \"fileToName\": \"\"\n },\n \"properties\": {\n  \"prop-int\": 22,\n  \"prop-null-bytes\": null\n },\n "
            "\"reliability\": \"NOT_PERSISTENT\",\n \"uuid\": 00000000-0000-0000-0000-000000000000\n}");
}

TEST_F(StreamMessageTest, testSetAndGet) {
  StreamMessage myMessage;

  tiny_mq::BytesVector data;
  data.push_back(2);
  data.push_back(4);
  data.push_back(8);
  data.push_back(16);
  data.push_back(32);

  EXPECT_NO_THROW(myMessage.write(false));
  EXPECT_NO_THROW(myMessage.write<byte>(127));
  EXPECT_NO_THROW(myMessage.write<character>('a'));
  EXPECT_NO_THROW(myMessage.write<short_integer>(32000));
  EXPECT_NO_THROW(myMessage.write<integer>(6789999));
  EXPECT_NO_THROW(myMessage.write<long_integer>(0xFFFAAA33345LL));
  EXPECT_NO_THROW(myMessage.write<floating_point>(0.000012F));
  EXPECT_NO_THROW(myMessage.write<double_point>(64.54654));
  EXPECT_NO_THROW(myMessage.write<tiny_mq::BytesVector>(data));

  myMessage.reset();

  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::BOOLEAN_TYPE);
  EXPECT_FALSE(myMessage.read<boolean>());
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::BYTE_TYPE);
  EXPECT_EQ(myMessage.read<byte>(), 127);
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::CHAR_TYPE);
  EXPECT_EQ(myMessage.read<character>(), 'a');
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::SHORT_TYPE);
  EXPECT_EQ(myMessage.read<short_integer>(), 32000);
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::INTEGER_TYPE);
  EXPECT_EQ(myMessage.read<integer>(), 6789999);
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::LONG_TYPE);
  EXPECT_EQ(myMessage.read<long_integer>(), 0xFFFAAA33345LL);
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::FLOAT_TYPE);
  EXPECT_EQ(myMessage.read<floating_point>(), 0.000012F);
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::DOUBLE_TYPE);
  EXPECT_EQ(myMessage.read<double_point>(), 64.54654);
  EXPECT_EQ(myMessage.nextValueType(), tiny_mq::property::BYTE_ARRAY_TYPE);
  EXPECT_EQ(myMessage.read<tiny_mq::BytesVector>(), data);
}

TEST_F(StreamMessageTest, testReadBoolean) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write(true));
  msg.reset();
  EXPECT_TRUE(msg.read<boolean>());
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadByte) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<byte>(42));
  msg.reset();
  EXPECT_EQ(msg.read<byte>(), 42);
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadShort) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<short_integer>(42));
  msg.reset();
  EXPECT_EQ(msg.read<short_integer>(), 42);
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadChar) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<character>('t'));
  msg.reset();
  EXPECT_EQ(msg.read<character>(), 't');
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadInt) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<integer>(42));
  msg.reset();
  EXPECT_EQ(msg.read<integer>(), 42);
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadLong) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<long_integer>(42L));
  msg.reset();
  EXPECT_EQ(msg.read<long_integer>(), 42L);
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadFloat) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<floating_point>(42.2F));
  msg.reset();
  EXPECT_EQ(msg.read<floating_point>(), 42.2F);
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadDouble) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<double_point>(42.2));
  msg.reset();
  EXPECT_EQ(msg.read<double_point>(), 42.2);
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadString) {
  StreamMessage msg;

  EXPECT_NO_THROW(msg.write<string>("hello tiny"));
  msg.reset();
  EXPECT_EQ(msg.read<string>(), std::string("hello tiny"));
  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<tiny_mq::BytesVector>(), Poco::Exception) << ("should have thrown exception");
}

TEST_F(StreamMessageTest, testReadBigString) {
  StreamMessage msg;

  // Test with a 1Meg String
  std::string bigString;
  bigString.reserve(1024 * 1024);
  for (int i = 0; i < 1024 * 1024; i++) {
    bigString.append(1, (char)'a' + i % 26);
  }

  EXPECT_NO_THROW(msg.write<string>(bigString));
  EXPECT_NO_THROW(msg.reset());
  EXPECT_EQ(bigString, msg.read<string>());
}

TEST_F(StreamMessageTest, testReadBytes) {
  StreamMessage msg;

  tiny_mq::BytesVector test(50);
  for (int8_t i = 0; i < 50; i++) {
    test[i] = i;
  }
  EXPECT_NO_THROW(msg.write<tiny_mq::BytesVector>(test));
  msg.reset();

  tiny_mq::BytesVector valid;
  EXPECT_NO_THROW(valid = msg.read<tiny_mq::BytesVector>());
  for (int i = 0; i < 50; i++) {
    EXPECT_TRUE(valid[i] == test[i]);
  }

  msg.reset();
  EXPECT_THROW(msg.read<double_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<boolean>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<byte>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<character>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<long_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<floating_point>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<short_integer>(), Poco::Exception) << ("should have thrown exception");
  msg.reset();
  EXPECT_THROW(msg.read<string>(), Poco::Exception) << ("should have thrown exception");
}