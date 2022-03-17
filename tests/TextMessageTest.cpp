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
  TextMessage msg(data);

  EXPECT_EQ(msg.text(), data);
  EXPECT_NO_THROW(msg.setProperty("prop-int", tiny_mq::property::Int(22)));
  EXPECT_NO_THROW(msg.setProperty("prop-null-bytes", tiny_mq::property::Bytes()));

  Poco::JSON::Object object;
  EXPECT_NO_THROW(object = msg.toJSON());
  std::stringstream ss;
  EXPECT_NO_THROW(object.stringify(ss, 1));
  std::string json = ss.str();
  EXPECT_EQ(json,
            "{\n \"data\": \"hello tiny-mq\",\n \"number\": 0,\n \"persistentInfo\": {\n  \"fileFromName\": \"\",\n  \"fileToName\": \"\"\n },\n "
            "\"properties\": {\n  \"prop-int\": 22,\n  \"prop-null-bytes\": null\n },\n \"reliability\": \"PERSISTENT\",\n \"uuid\": "
            "00000000-0000-0000-0000-000000000000\n}");
}