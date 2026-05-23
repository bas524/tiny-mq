//
// Created by Alexander Bychuk on 05.03.2022.
//

#include "TextMessageTest.h"
#include "TextMessage.h"

using tiny_mq::TextMessage;

TEST_F(TextMessageTest, testClearBody) {
  std::string testText = "This is some test Text";

  TextMessage textMessage;

  textMessage.setText(testText);

  EXPECT_EQ(textMessage.text(), testText);

  textMessage.clearData();

  EXPECT_TRUE(textMessage.text().empty());
}

TEST_F(TextMessageTest, testToJson) {
  std::string data = "hello tiny-mq";
  TextMessage msg({}, data);

  EXPECT_EQ(msg.text(), data);
  EXPECT_NO_THROW(msg.setProperty("prop-int", tiny_mq::property::Int(22)));
  EXPECT_NO_THROW(msg.setProperty("prop-null-bytes", tiny_mq::property::Bytes()));
  std::string json;
  EXPECT_NO_THROW(json = tiny_mq::message::dump(msg));
  EXPECT_EQ(json,
            "{\n \"data\": \"hello tiny-mq\",\n \"number\": 0,\n \"persistentInfo\": {\n  \"payload\": {\n   \"dataPath\": \"\",\n   "
            "\"propertiesPath\": \"\"\n  },\n  \"sent\": {\n   \"dataPath\": \"\",\n   \"propertiesPath\": \"\"\n  },\n  \"transaction\": {\n   "
            "\"dataPath\": \"\",\n   \"propertiesPath\": \"\"\n  }\n },\n \"properties\": {\n  \"prop-int\": {\n   \"type\": \"INTEGER\",\n   "
            "\"value\": 22\n  },\n  \"prop-null-bytes\": {\n   \"type\": \"BYTE_ARRAY\",\n   \"value\": null\n  }\n },\n \"reliability\": "
            "\"NOT_PERSISTENT\",\n \"uuid\": \"00000000-0000-0000-0000-000000000000\"\n}");
}
