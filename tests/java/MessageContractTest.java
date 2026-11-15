import jakarta.jms.BytesMessage;
import jakarta.jms.DeliveryMode;
import jakarta.jms.JMSException;
import jakarta.jms.MapMessage;
import jakarta.jms.Message;
import jakarta.jms.MessageEOFException;
import jakarta.jms.MessageFormatException;
import jakarta.jms.MessageNotReadableException;
import jakarta.jms.MessageNotWriteableException;
import jakarta.jms.ObjectMessage;
import jakarta.jms.Queue;
import jakarta.jms.StreamMessage;
import jakarta.jms.TextMessage;
import tinymq.jms.JavaMessageAdapter;
import tinymq.jms.MessageEnvelope;
import tinymq.jms.TinyBytesMessage;
import tinymq.jms.TinyMapMessage;
import tinymq.jms.TinyMessage;
import tinymq.jms.TinyObjectMessage;
import tinymq.jms.TinyStreamMessage;
import tinymq.jms.TinyTextMessage;

import java.io.IOException;
import java.io.Serializable;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;

public final class MessageContractTest {
    private final Map<String, String[]> fixture = new HashMap<>();

    private MessageContractTest(Path input) throws IOException {
        for (String line : Files.readAllLines(input)) {
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] fields = line.split("\t", -1);
            check(fields.length == 3, "invalid fixture line " + line);
            check(fixture.putIfAbsent(fields[0], fields) == null, "duplicate fixture entry");
        }
    }

    public static void main(String[] args) throws Exception {
        MessageContractTest test = new MessageContractTest(Path.of(args[0]));
        run("MessageContractTest.CrossLanguageEnvelope", "REQ-70-1", test::crossLanguageEnvelope);
        run("MessageContractTest.BodyRoundTrip", "REQ-70-2", test::bodyRoundTrip);
        run("MessageContractTest.BodyAccessMatrix", "REQ-70-3", test::bodyAccessMatrix);
        run("MessageContractTest.PropertyAccessMatrix", "REQ-70-4", test::propertyAccessMatrix);
        run("MessageContractTest.GroupProperties", "REQ-70-7", test::groupProperties);
        run("MessageContractTest.BinaryCorrelationId", "REQ-70-8", test::binaryCorrelationId);
    }

    private interface Checked { void run() throws Exception; }

    private static void run(String name, String requirement, Checked body) throws Exception {
        System.out.println("[ RUN      ] " + name + " (" + requirement + ")");
        body.run();
        System.out.println("[       OK ] " + name + " (" + requirement + ")");
    }

    private static void check(boolean condition, String context) {
        if (!condition) throw new AssertionError(context);
    }

    private static void equal(Object expected, Object actual) {
        check(Objects.deepEquals(expected, actual), "expected " + expected + ", got " + actual);
    }

    private static void expect(Class<? extends Exception> expected, Checked action) throws Exception {
        try {
            action.run();
        } catch (Exception actual) {
            if (actual.getClass() == expected) return;
            throw new AssertionError("expected " + expected + ", got " + actual, actual);
        }
        throw new AssertionError("expected " + expected + ", but call succeeded");
    }

    private String field(String key, String type) {
        String[] row = fixture.get(key);
        equal(type, row[1]);
        return row[2];
    }

    private int integer(String key) { return Integer.parseInt(field(key, "int")); }

    private long longValue(String key) { return Long.parseLong(field(key, "long")); }

    private byte[] bytes(String key) {
        String value = field(key, "bytes");
        byte[] result = new byte[value.length() / 2];
        for (int i = 0; i < result.length; ++i) {
            result[i] = (byte) Integer.parseInt(value.substring(i * 2, i * 2 + 2), 16);
        }
        return result;
    }

    private MessageEnvelope snapshot(TinyMessage message) throws JMSException {
        return JavaMessageAdapter.snapshotForSend(message, "ID:fixture", 1L, (Queue) () -> field("destination", "uri"),
                DeliveryMode.PERSISTENT, integer("priority"), longValue("expiration"), longValue("deliveryTime"));
    }

    private void crossLanguageEnvelope() throws Exception {
        Queue destination = () -> field("destination", "uri");
        Queue reply = () -> field("replyTo", "uri");
        TinyMessage message = new TinyTextMessage(field("text", "string"));
        message.setJMSReplyTo(reply);
        message.setJMSCorrelationID(field("correlation", "string"));
        message.setJMSType(field("messageType", "string"));
        String previous = null;
        for (int hints : new int[]{0, 1, 2, 3, 0}) {
            message.setJMSDestination((Queue) () -> "client value");
            message.setJMSMessageID("client-id");
            message.setJMSTimestamp(-1L);
            message.setJMSPriority(1);
            message.setJMSExpiration(-1L);
            message.setJMSDeliveryTime(-1L);
            message.setJMSDeliveryMode(DeliveryMode.NON_PERSISTENT);
            message.setJMSRedelivered(true);
            message.setIntProperty("JMSXDeliveryCount", 99);
            long begin = System.currentTimeMillis();
            String id = (hints & 1) == 0 ? "ID:" + UUID.randomUUID() : null;
            long timestamp = (hints & 2) == 0 ? System.currentTimeMillis() : 0;
            MessageEnvelope envelope = JavaMessageAdapter.snapshotForSend(message, id, timestamp, destination,
                    integer("deliveryMode"), integer("priority"), longValue("expiration"), longValue("deliveryTime"));
            long end = System.currentTimeMillis();
            int[] acks = {0};
            Message view = JavaMessageAdapter.forDelivery(envelope, false, integer("firstDeliveryCount"), () -> ++acks[0]);
            equal(id, view.getJMSMessageID());
            equal(id, message.getJMSMessageID());
            equal(timestamp, view.getJMSTimestamp());
            if ((hints & 1) == 0) {
                check(id.startsWith(field("messageIdPrefix", "string")), "ID prefix");
                check(!id.equals(previous), "provider assigned a duplicate ID");
                previous = id;
            }
            if ((hints & 2) == 0) {
                check(timestamp >= begin && timestamp <= end, "timestamp outside send");
            }
            equal(field("destination", "uri"), ((Queue) view.getJMSDestination()).getQueueName());
            equal(field("replyTo", "uri"), ((Queue) view.getJMSReplyTo()).getQueueName());
            equal(field("messageType", "string"), view.getJMSType());
            equal(field("correlation", "string"), view.getJMSCorrelationID());
            equal(integer("deliveryMode"), view.getJMSDeliveryMode());
            equal(integer("priority"), view.getJMSPriority());
            equal(longValue("expiration"), view.getJMSExpiration());
            equal(longValue("deliveryTime"), view.getJMSDeliveryTime());
            equal(false, view.getJMSRedelivered());
            equal(integer("firstDeliveryCount"), view.getObjectProperty("JMSXDeliveryCount"));
            view.acknowledge();
            equal(1, acks[0]);
            Message second = JavaMessageAdapter.forDelivery(envelope, true, integer("repeatDeliveryCount"), null);
            equal(true, second.getJMSRedelivered());
            equal(integer("repeatDeliveryCount"), second.getObjectProperty("JMSXDeliveryCount"));
        }
    }

    private static final class Broken implements Serializable {
        private static final long serialVersionUID = 1L;
        private final Object value = new Object();
    }

    private void bodyRoundTrip() throws Exception {
        byte[] original = bytes("binaryCorrelation");
        TinyBytesMessage binary = new TinyBytesMessage();
        binary.writeBytes(original);
        expect(NullPointerException.class, () -> binary.writeObject(null));
        TinyMapMessage map = new TinyMapMessage();
        map.setBytes("bytes", original);
        map.setString("nil", null);
        TinyStreamMessage stream = new TinyStreamMessage();
        stream.writeBytes(original);
        stream.writeObject(null);
        original[0] = 17;

        ArrayList<String> object = new ArrayList<>(List.of("set"));
        TinyObjectMessage objectMessage = new TinyObjectMessage();
        objectMessage.setObject(object);
        TinyObjectMessage constructed = new TinyObjectMessage(object);
        object.add("before snapshot");
        expect(MessageFormatException.class, () -> objectMessage.setObject(new Broken()));
        expect(MessageFormatException.class, () -> new TinyObjectMessage(new Broken()));
        MessageEnvelope snapshot = snapshot(objectMessage);
        object.add("after snapshot");
        equal(List.of("set"), objectMessage.getObject());
        equal(List.of("set"), constructed.getObject());
        equal(List.of("set"), ((ObjectMessage) JavaMessageAdapter.forDelivery(snapshot, false, 1, null)).getObject());

        TinyTextMessage text = new TinyTextMessage(field("text", "string"));
        text.setStringProperty("original", "fixed");
        MessageEnvelope textSnapshot = snapshot(text);
        text.setText("outbound modified");
        TinyMessage delivered = JavaMessageAdapter.forDelivery(textSnapshot, false, 1, null);
        equal(field("text", "string"), delivered.getBody(String.class));
        delivered.clearBody();
        ((TextMessage) delivered).setText("recipient modified");
        delivered.clearProperties();
        delivered.setStringProperty("original", "overwritten");
        TinyMessage repeat = JavaMessageAdapter.forDelivery(textSnapshot, true, 2, null);
        equal("fixed", repeat.getStringProperty("original"));
        equal(field("text", "string"), repeat.getBody(String.class));

        equal(null, JavaMessageAdapter.forDelivery(snapshot(new TinyMessage()), false, 1, null).getBody(Object.class));
        for (String value : new String[]{null, ""}) {
            equal(value, ((TextMessage) JavaMessageAdapter.forDelivery(snapshot(new TinyTextMessage(value)), false, 1, null)).getText());
        }
        byte[] expected = bytes("binaryCorrelation");
        equal(expected, JavaMessageAdapter.forDelivery(snapshot(binary), false, 1, null).getBody(byte[].class));
        MapMessage mapped = (MapMessage) JavaMessageAdapter.forDelivery(snapshot(map), false, 1, null);
        equal(expected, mapped.getBytes("bytes"));
        check(mapped.itemExists("nil"), "map null lost");
        equal(null, mapped.getObject("nil"));
        StreamMessage streamed = (StreamMessage) JavaMessageAdapter.forDelivery(snapshot(stream), false, 1, null);
        equal(expected, streamed.readObject());
        equal(null, streamed.readObject());

        Message emptyBytes = JavaMessageAdapter.forDelivery(snapshot(new TinyBytesMessage()), false, 1, null);
        equal(0L, ((BytesMessage) emptyBytes).getBodyLength());
        equal(null, emptyBytes.getBody(byte[].class));
        Message emptyMap = JavaMessageAdapter.forDelivery(snapshot(new TinyMapMessage()), false, 1, null);
        equal(null, emptyMap.getBody(Map.class));
        equal(false, ((MapMessage) emptyMap).getMapNames().hasMoreElements());
        Message emptyStream = JavaMessageAdapter.forDelivery(snapshot(new TinyStreamMessage()), false, 1, null);
        expect(MessageEOFException.class, ((StreamMessage) emptyStream)::readObject);
        Message emptyObject = JavaMessageAdapter.forDelivery(snapshot(new TinyObjectMessage((Serializable) null)),
                false, 1, null);
        equal(null, ((ObjectMessage) emptyObject).getObject());
        equal(null, emptyObject.getBody(Serializable.class));
    }

    private void bodyAccessMatrix() throws Exception {
        conversionMatrix();
        nullBodyValues();
        bytesObjectWrites();
        TinyStreamMessage stream = new TinyStreamMessage();
        stream.writeString("not-a-number");
        stream.writeInt(12);
        stream.writeBytes(new byte[]{1, 2, 3});
        stream.writeChar('z');
        stream.writeBytes(new byte[]{4, 5});
        stream.writeBytes(new byte[0]);
        stream.writeObject(null);
        stream.writeString("end");
        expect(MessageNotReadableException.class, stream::readString);
        stream.reset();
        expect(NumberFormatException.class, stream::readInt);
        equal("not-a-number", stream.readString());
        expect(MessageFormatException.class, stream::readDouble);
        equal(12L, stream.readLong());
        byte[] chunk = new byte[2];
        equal(2, stream.readBytes(chunk));
        equal(new byte[]{1, 2}, chunk);
        expect(MessageFormatException.class, stream::readObject);
        equal(1, stream.readBytes(chunk));
        equal('z', stream.readChar());
        equal(2, stream.readBytes(chunk));
        expect(MessageFormatException.class, stream::readObject);
        equal(-1, stream.readBytes(chunk));
        equal(0, stream.readBytes(chunk));
        equal(-1, stream.readBytes(chunk));
        equal("end", stream.readString());
        expect(MessageEOFException.class, stream::readString);
        expect(MessageNotWriteableException.class, () -> stream.writeString("new"));
        expect(MessageFormatException.class, () -> stream.getBody(Object.class));
        equal(false, stream.isBodyAssignableTo(Object.class));

        TinyBytesMessage binary = new TinyBytesMessage();
        binary.writeBytes(new byte[]{0, 1, (byte) 0x80, 42});
        expect(MessageNotReadableException.class, binary::readByte);
        binary.reset();
        expect(MessageFormatException.class, binary::readUTF);
        equal((short) 1, binary.readShort());
        equal(128, binary.readUnsignedByte());
        equal(false, binary.isBodyAssignableTo(String.class));
        expect(MessageFormatException.class, () -> binary.getBody(String.class));
        equal((byte) 0, binary.readByte());
        equal(new byte[]{0, 1, (byte) 0x80, 42}, binary.getBody(Object.class));
        equal((byte) 0, binary.readByte());
        expect(MessageEOFException.class, binary::readLong);
        equal((byte) 1, binary.readByte());
        expect(MessageNotWriteableException.class, () -> binary.writeByte((byte) 2));
        binary.clearBody();
        equal(null, binary.getBody(String.class));

        TinyMapMessage map = new TinyMapMessage();
        map.setBytes("value", new byte[]{1, 2, 3}, 1, 2);
        byte[] copy = map.getBytes("value");
        copy[0] = 7;
        equal(new byte[]{2, 3}, map.getBytes("value"));
        map.setByte("b", (byte) 8);
        equal(8, map.getInt("b"));
        equal("8", map.getString("b"));
        expect(MessageFormatException.class, () -> map.getFloat("b"));
        expect(MessageFormatException.class, () -> map.getString("value"));
        expect(MessageFormatException.class, () -> map.setObject("list", new ArrayList<>()));
        expect(MessageFormatException.class, () -> map.getBody(LinkedHashMap.class));
        check(map.getBody(Map.class).containsKey("value"), "Map body unavailable");
        TinyMapMessage received = (TinyMapMessage) JavaMessageAdapter.forDelivery(snapshot(map), false, 1, null);
        expect(MessageNotWriteableException.class, () -> received.setInt("i", 1));
        received.clearBody();
        received.setInt("i", 9);
        equal(9, received.getInt("i"));

        TinyTextMessage text = new TinyTextMessage("text");
        text.setStringProperty("tag", "x");
        text.setJMSType("event");
        TinyTextMessage delivered = (TinyTextMessage) JavaMessageAdapter.forDelivery(snapshot(text), false, 1, null);
        equal("text", delivered.getBody(CharSequence.class));
        expect(MessageFormatException.class, () -> delivered.getBody(Integer.class));
        expect(MessageNotWriteableException.class, () -> delivered.setText("new"));
        delivered.clearBody();
        equal(null, delivered.getBody(Integer.class));
        equal("x", delivered.getStringProperty("tag"));
        equal("event", delivered.getJMSType());
        delivered.setText("new");

        MessageEnvelope malformed = new MessageEnvelope(MessageEnvelope.Kind.OBJECT, null, 0, null, null, null,
                DeliveryMode.NON_PERSISTENT, null, 0, 0, 4, Map.of(), new byte[]{0, 1}, "legacy.Hint");
        ObjectMessage object = (ObjectMessage) JavaMessageAdapter.forDelivery(malformed, false, 1, null);
        equal("legacy.Hint", snapshot((TinyObjectMessage) object).objectClassName);
        expect(MessageFormatException.class, object::getObject);
        expect(MessageFormatException.class, () -> object.getBody(Object.class));
        equal(false, object.isBodyAssignableTo(Object.class));
    }

    @FunctionalInterface
    private interface Getter { Object read(Message msg, String name) throws Exception; }

    private void nullBodyValues() throws Exception {
        TinyMapMessage map = new TinyMapMessage();
        map.setObject("nil", null);
        for (String name : List.of("nil", "missing")) {
            equal(false, map.getBoolean(name));
            equal(null, map.getString(name));
            equal(null, map.getBytes(name));
            expect(NumberFormatException.class, () -> map.getByte(name));
            expect(NumberFormatException.class, () -> map.getShort(name));
            expect(NumberFormatException.class, () -> map.getInt(name));
            expect(NumberFormatException.class, () -> map.getLong(name));
            expect(NullPointerException.class, () -> map.getChar(name));
            expect(NullPointerException.class, () -> map.getFloat(name));
            expect(NullPointerException.class, () -> map.getDouble(name));
        }
        TinyStreamMessage stream = new TinyStreamMessage();
        stream.writeObject(null);
        stream.reset();
        expect(NumberFormatException.class, stream::readInt);
        expect(NullPointerException.class, stream::readChar);
        equal(null, stream.readString());
        expect(MessageEOFException.class, stream::readObject);
    }

    private void bytesObjectWrites() throws Exception {
        TinyBytesMessage bytes = new TinyBytesMessage();
        for (Object value : List.of(true, (byte) -7, (short) -12, '字', 13, 14L, 2.5f, 3.25,
                "text", new byte[]{0, (byte) 255})) {
            bytes.writeObject(value);
        }
        expect(MessageFormatException.class, () -> bytes.writeObject(new Object()));
        bytes.reset();
        equal(true, bytes.readBoolean());
        equal((byte) -7, bytes.readByte());
        equal((short) -12, bytes.readShort());
        equal('字', bytes.readChar());
        equal(13, bytes.readInt());
        equal(14L, bytes.readLong());
        equal(2.5f, bytes.readFloat());
        equal(3.25, bytes.readDouble());
        equal("text", bytes.readUTF());
        equal((byte) 0, bytes.readByte());
        equal(255, bytes.readUnsignedByte());
        expect(MessageEOFException.class, bytes::readByte);
    }

    private void conversionMatrix() throws Exception {
        Object[] values = {true, (byte) 12, (short) 12, '字', 12, 12L, 12.0f, 12.0, "12", new byte[]{42}};
        Getter[] mapReaders = {
                (m, n) -> ((MapMessage) m).getBoolean(n), (m, n) -> ((MapMessage) m).getByte(n),
                (m, n) -> ((MapMessage) m).getShort(n), (m, n) -> ((MapMessage) m).getChar(n),
                (m, n) -> ((MapMessage) m).getInt(n), (m, n) -> ((MapMessage) m).getLong(n),
                (m, n) -> ((MapMessage) m).getFloat(n), (m, n) -> ((MapMessage) m).getDouble(n),
                (m, n) -> ((MapMessage) m).getString(n), (m, n) -> ((MapMessage) m).getBytes(n)};
        Getter[] streamReaders = {
                (m, n) -> ((StreamMessage) m).readBoolean(), (m, n) -> ((StreamMessage) m).readByte(),
                (m, n) -> ((StreamMessage) m).readShort(), (m, n) -> ((StreamMessage) m).readChar(),
                (m, n) -> ((StreamMessage) m).readInt(), (m, n) -> ((StreamMessage) m).readLong(),
                (m, n) -> ((StreamMessage) m).readFloat(), (m, n) -> ((StreamMessage) m).readDouble(),
                (m, n) -> ((StreamMessage) m).readString(), (m, n) -> {
                    byte[] buffer = new byte[2];
                    equal(1, ((StreamMessage) m).readBytes(buffer));
                    return new byte[]{buffer[0]};
                }};
        // Jakarta Messaging §3.11.3. Each row is the source, each column is the requested type.
        String[] accepted = {"y-------y-", "-yy-yy--y-", "--y-yy--y-", "---y----y-", "----yy--y-",
                             "-----y--y-", "------yyy-", "-------yy-", "yyy-yyyyy-", "---------y"};
        TinyMapMessage map = new TinyMapMessage();
        TinyStreamMessage stream = new TinyStreamMessage();
        for (int from = 0; from < values.length; ++from) {
            Object value = values[from];
            map.setObject("value", value);
            for (int to = 0; to < values.length; ++to) {
                stream.clearBody();
                stream.writeObject(value);
                stream.reset();
                Getter mapReader = mapReaders[to];
                Getter streamReader = streamReaders[to];
                if (accepted[from].charAt(to) == 'y') {
                    Object expected = to == 8 ? value.toString() : to == 0 && from == 8 ? false : values[to];
                    equal(expected, mapReader.read(map, "value"));
                    equal(expected, streamReader.read(stream, "value"));
                    expect(MessageEOFException.class, stream::readObject);
                } else {
                    expect(MessageFormatException.class, () -> mapReader.read(map, "value"));
                    expect(MessageFormatException.class, () -> streamReader.read(stream, "value"));
                    equal(value, stream.readObject());
                }
            }
        }
    }

    private void propertyAccessMatrix() throws Exception {
        TinyTextMessage message = new TinyTextMessage("body");
        Object[] values = {true, (byte) 12, (short) 12, 12, 12L, 12.0f, 12.0, "12"};
        Getter[] getters = {Message::getBooleanProperty, Message::getByteProperty, Message::getShortProperty,
                Message::getIntProperty, Message::getLongProperty, Message::getFloatProperty,
                Message::getDoubleProperty, Message::getStringProperty};
        // Explicit table for Jakarta Messaging §3.5.4, rows are source types.
        String[] expectedConversions = {"y------y", "-yyyy--y", "--yyy--y", "---yy--y",
                "----y--y", "-----yyy", "------yy", "yyyyyyyy"};
        for (int from = 0; from < values.length; ++from) {
            Object value = values[from];
            message.setObjectProperty("p", value);
            equal(value, message.getObjectProperty("p"));
            for (int to = 0; to < getters.length; ++to) {
                Getter getter = getters[to];
                if (expectedConversions[from].charAt(to) == '-') {
                    expect(MessageFormatException.class, () -> getter.read(message, "p"));
                } else {
                    Object expected = to == 7 ? value.toString() : to == 0 && from == 7 ? false : values[to];
                    equal(expected, getter.read(message, "p"));
                }
            }
        }
        String explicitNull = field("nullProperty", "name");
        String missing = field("missing", "name");
        message.setStringProperty(explicitNull, null);
        check(message.propertyExists(explicitNull), "explicit null is absent");
        check(!message.propertyExists(missing), "missing property is present");
        check(Collections.list(message.getPropertyNames()).contains(explicitNull), "null missing from names");
        check(!Collections.list(message.getPropertyNames()).contains(missing), "absent name enumerated");
        for (String name : List.of(explicitNull, missing)) {
            equal(null, message.getStringProperty(name));
            equal(null, message.getObjectProperty(name));
            equal(false, message.getBooleanProperty(name));
            expect(NumberFormatException.class, () -> message.getByteProperty(name));
            expect(NumberFormatException.class, () -> message.getShortProperty(name));
            expect(NumberFormatException.class, () -> message.getIntProperty(name));
            expect(NumberFormatException.class, () -> message.getLongProperty(name));
            expect(NullPointerException.class, () -> message.getFloatProperty(name));
            expect(NullPointerException.class, () -> message.getDoubleProperty(name));
        }
        message.setStringProperty("bad", "no numeric value");
        expect(NumberFormatException.class, () -> message.getIntProperty("bad"));
        expect(NumberFormatException.class, () -> message.getDoubleProperty("bad"));
        for (String name : Arrays.asList(null, "", "two words", "a.b", "with-dash", "3x", "LIKE", "eScApE", "fAlSe", "JMSType")) {
            expect(IllegalArgumentException.class, () -> message.setBooleanProperty(name, true));
        }
        message.setIntProperty("$π1", 6);
        message.setIntProperty("jMsXGroupSeq", 11);
        message.setStringProperty("JMS_vendor", "value");
        expect(MessageFormatException.class, () -> message.setObjectProperty("bytes", new byte[]{1}));
        expect(MessageFormatException.class, () -> message.setObjectProperty("char", 'c'));
        equal(6, message.getIntProperty("$π1"));
        check(!message.propertyExists("$Π1"), "property names are case-sensitive");
        TinyMessage received = JavaMessageAdapter.forDelivery(snapshot(message), false, 1, null);
        expect(MessageNotWriteableException.class, () -> received.setIntProperty("p", 123));
        received.clearProperties();
        check(!received.propertyExists(explicitNull), "clearProperties kept original property");
        check(!received.getPropertyNames().hasMoreElements(), "clearProperties kept property names");
        equal("body", received.getBody(String.class));
        equal("ID:fixture", received.getJMSMessageID());
        received.setLongProperty("p", 123L);
        equal(123L, received.getObjectProperty("p"));
    }

    private void groupProperties() throws Exception {
        TinyMessage source = new TinyMessage();
        Message absent = JavaMessageAdapter.forDelivery(snapshot(source), false, 1, null);
        check(!absent.propertyExists("JMSXGroupID"), "unexpected group ID");
        check(!absent.propertyExists("JMSXGroupSeq"), "unexpected group sequence");
        source.setStringProperty("JMSXGroupID", field("groupID", "string"));
        source.setIntProperty("JMSXGroupSeq", integer("groupSeq"));
        Message delivered = JavaMessageAdapter.forDelivery(snapshot(source), false, 1, null);
        equal(field("groupID", "string"), delivered.getObjectProperty("JMSXGroupID"));
        equal(integer("groupSeq"), delivered.getObjectProperty("JMSXGroupSeq"));
    }

    private void binaryCorrelationId() throws Exception {
        TinyMessage message = new TinyMessage();
        Message absent = JavaMessageAdapter.forDelivery(snapshot(message), false, 1, null);
        equal(null, absent.getJMSCorrelationID());
        equal(null, absent.getJMSCorrelationIDAsBytes());
        for (String value : new String[]{"", field("correlation", "string")}) {
            message.setJMSCorrelationID(value);
            Message delivered = JavaMessageAdapter.forDelivery(snapshot(message), false, 1, null);
            equal(value, delivered.getJMSCorrelationID());
            expect(MessageFormatException.class, delivered::getJMSCorrelationIDAsBytes);
        }
        for (byte[] value : new byte[][]{new byte[0], bytes("binaryCorrelation")}) {
            byte[] input = value.clone();
            message.setJMSCorrelationIDAsBytes(input);
            if (input.length > 0) input[0] = 17;
            MessageEnvelope envelope = snapshot(message);
            byte[] output = (byte[]) envelope.correlationId();
            if (output.length > 0) output[0] = 18;
            Message delivered = JavaMessageAdapter.forDelivery(envelope, false, 1, null);
            equal(value, delivered.getJMSCorrelationIDAsBytes());
            expect(MessageFormatException.class, delivered::getJMSCorrelationID);
            byte[] fetched = delivered.getJMSCorrelationIDAsBytes();
            if (fetched.length > 0) fetched[0] = 19;
            equal(value, delivered.getJMSCorrelationIDAsBytes());
        }
        message.setJMSCorrelationIDAsBytes(null);
        equal(null, message.getJMSCorrelationID());
        equal(null, message.getJMSCorrelationIDAsBytes());
    }
}
