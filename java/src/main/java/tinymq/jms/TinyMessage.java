package tinymq.jms;

import jakarta.jms.Destination;
import jakarta.jms.JMSException;
import jakarta.jms.Message;
import jakarta.jms.MessageFormatException;
import jakarta.jms.MessageNotWriteableException;

import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

public class TinyMessage implements Message {
    private static final Set<String> KEYWORDS =
            Set.of("NULL", "TRUE", "FALSE", "NOT", "AND", "OR", "BETWEEN", "LIKE", "IN", "IS", "ESCAPE");
    private static final Set<String> HEADERS = Set.of("JMSMessageID", "JMSTimestamp", "JMSCorrelationID",
            "JMSReplyTo", "JMSDestination", "JMSDeliveryMode", "JMSRedelivered", "JMSType",
            "JMSExpiration", "JMSDeliveryTime", "JMSPriority");
    final Map<String, Object> properties = new LinkedHashMap<>();
    private boolean propertiesReadOnly;
    private boolean bodyReadOnly;
    private JavaMessageAdapter.Acknowledgement acknowledgement;

    private String messageID;
    private long timestamp;
    private Object correlationID;
    private Destination replyTo;
    private Destination destination;
    private int deliveryMode = DEFAULT_DELIVERY_MODE;
    private boolean redelivered;
    private String type;
    private long expiration;
    private long deliveryTime;
    private int priority = DEFAULT_PRIORITY;

    public byte[] getJMSCorrelationIDAsBytes() throws MessageFormatException {
        if (correlationID instanceof String) throw new MessageFormatException("Correlation ID is a String");
        return (byte[]) copyValue(correlationID);
    }

    public void setJMSCorrelationIDAsBytes(byte[] value) {
        correlationID = copyValue(value);
    }

    public String getJMSCorrelationID() throws MessageFormatException {
        if (correlationID instanceof byte[]) throw new MessageFormatException("Correlation ID is binary");
        return (String) correlationID;
    }

    public void setJMSCorrelationID(String value) {
        correlationID = value;
    }

    @Override
    public void clearProperties() {
        properties.clear();
        propertiesReadOnly = false;
    }

    @Override
    public boolean propertyExists(String name) {
        return properties.containsKey(name);
    }

    @Override
    public Enumeration<String> getPropertyNames() {
        return Collections.enumeration(properties.keySet());
    }

    @Override
    public Object getObjectProperty(String name) {
        return properties.get(name);
    }

    @Override
    public void setObjectProperty(String name, Object value) throws JMSException {
        if (propertiesReadOnly) throw new MessageNotWriteableException("Properties are read-only");
        if (!validPropertyName(name)) {
            throw new IllegalArgumentException("Invalid property name: " + name);
        }
        checkValue(value, false);
        properties.put(name, value);
    }

    @Override
    public void acknowledge() throws JMSException {
        if (acknowledgement != null) acknowledgement.acknowledge();
    }

    @Override
    public void clearBody() {
        discardBody();
        bodyReadOnly = false;
    }

    @Override
    public <T> T getBody(Class<T> requested) throws JMSException {
        Object value = bodyValue();
        if (value == null || requested.isInstance(value)) return requested.cast(value);
        throw new MessageFormatException("Body is not assignable to " + requested.getTypeName());
    }

    @Override
    public boolean isBodyAssignableTo(Class requested) throws JMSException {
        try {
            Object value = bodyValue();
            return value == null || requested.isInstance(value);
        } catch (MessageFormatException failure) {
            return false;
        }
    }

    Object snapshotBody() { return null; }

    Object bodyValue() throws JMSException { return snapshotBody(); }

    void discardBody() {}

    void makeBodyReadable() { bodyReadOnly = true; }

    final void makeReceived(JavaMessageAdapter.Acknowledgement callback) {
        makeBodyReadable();
        propertiesReadOnly = true;
        acknowledgement = callback;
    }

    final boolean bodyReadOnly() { return bodyReadOnly; }

    final void requireBodyWriteable() throws MessageNotWriteableException {
        if (bodyReadOnly) throw new MessageNotWriteableException("Body is read-only");
    }

    final Object correlationValue() { return correlationID; }

    final void restoreCorrelation(Object value) { correlationID = copyValue(value); }

    static boolean validPropertyName(String name) {
        return name != null && !name.isEmpty() && Character.isJavaIdentifierStart(name.codePointAt(0))
                && name.codePoints().skip(1).allMatch(Character::isJavaIdentifierPart)
                && !KEYWORDS.contains(name.toUpperCase(Locale.ROOT)) && !HEADERS.contains(name);
    }

    static Object copyValue(Object value) {
        return value instanceof byte[] ? ((byte[]) value).clone() : value;
    }

    static void checkValue(Object value, boolean body) throws MessageFormatException {
        if (value == null || value instanceof String || value instanceof Boolean || value instanceof Byte
                || value instanceof Short || value instanceof Integer || value instanceof Long
                || value instanceof Float || value instanceof Double
                || (body && (value instanceof Character || value instanceof byte[]))) {
            return;
        }
        throw new MessageFormatException("Unsupported value type: " + value.getClass().getTypeName());
    }

    static Object convert(Object value, Class<?> requested) throws MessageFormatException {
        if (requested == Object.class || requested.isInstance(value)) return copyValue(value);
        if (requested == String.class && !(value instanceof byte[])) return value == null ? null : value.toString();
        if (requested == byte[].class && value == null) return null;
        if (requested == Character.class && value == null) throw new NullPointerException("Null character");
        if (value instanceof String || value == null) {
            String text = (String) value;
            if (requested == Boolean.class) return Boolean.valueOf(text);
            if (requested == Byte.class) return Byte.valueOf(text);
            if (requested == Short.class) return Short.valueOf(text);
            if (requested == Integer.class) return Integer.valueOf(text);
            if (requested == Long.class) return Long.valueOf(text);
            if (requested == Float.class) return Float.valueOf(text);
            if (requested == Double.class) return Double.valueOf(text);
        }
        if (value instanceof Byte || value instanceof Short || value instanceof Integer) {
            Number number = (Number) value;
            if (requested == Short.class && value instanceof Byte) return number.shortValue();
            if (requested == Integer.class && !(value instanceof Integer)) return number.intValue();
            if (requested == Long.class) return number.longValue();
        }
        if (value instanceof Float && requested == Double.class) return ((Float) value).doubleValue();
        throw new MessageFormatException("Value is not convertible to " + requested.getTypeName());
    }

    @Override
    public String getJMSMessageID() { return messageID; }

    @Override
    public void setJMSMessageID(String value) { messageID = value; }

    @Override
    public long getJMSTimestamp() { return timestamp; }

    @Override
    public void setJMSTimestamp(long value) { timestamp = value; }

    @Override
    public Destination getJMSReplyTo() { return replyTo; }

    @Override
    public void setJMSReplyTo(Destination value) { replyTo = value; }

    @Override
    public Destination getJMSDestination() { return destination; }

    @Override
    public void setJMSDestination(Destination value) { destination = value; }

    @Override
    public int getJMSDeliveryMode() { return deliveryMode; }

    @Override
    public void setJMSDeliveryMode(int value) { deliveryMode = value; }

    @Override
    public boolean getJMSRedelivered() { return redelivered; }

    @Override
    public void setJMSRedelivered(boolean value) { redelivered = value; }

    @Override
    public String getJMSType() { return type; }

    @Override
    public void setJMSType(String value) { type = value; }

    @Override
    public long getJMSExpiration() { return expiration; }

    @Override
    public void setJMSExpiration(long value) { expiration = value; }

    @Override
    public long getJMSDeliveryTime() { return deliveryTime; }

    @Override
    public void setJMSDeliveryTime(long value) { deliveryTime = value; }

    @Override
    public int getJMSPriority() { return priority; }

    @Override
    public void setJMSPriority(int value) { priority = value; }

    @Override
    public boolean getBooleanProperty(String name) throws MessageFormatException {
        return (Boolean) convert(properties.get(name), Boolean.class);
    }

    @Override
    public void setBooleanProperty(String name, boolean value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public byte getByteProperty(String name) throws MessageFormatException {
        return (Byte) convert(properties.get(name), Byte.class);
    }

    @Override
    public void setByteProperty(String name, byte value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public short getShortProperty(String name) throws MessageFormatException {
        return (Short) convert(properties.get(name), Short.class);
    }

    @Override
    public void setShortProperty(String name, short value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public int getIntProperty(String name) throws MessageFormatException {
        return (Integer) convert(properties.get(name), Integer.class);
    }

    @Override
    public void setIntProperty(String name, int value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public long getLongProperty(String name) throws MessageFormatException {
        return (Long) convert(properties.get(name), Long.class);
    }

    @Override
    public void setLongProperty(String name, long value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public float getFloatProperty(String name) throws MessageFormatException {
        return (Float) convert(properties.get(name), Float.class);
    }

    @Override
    public void setFloatProperty(String name, float value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public double getDoubleProperty(String name) throws MessageFormatException {
        return (Double) convert(properties.get(name), Double.class);
    }

    @Override
    public void setDoubleProperty(String name, double value) throws JMSException {
        setObjectProperty(name, value);
    }

    @Override
    public String getStringProperty(String name) throws MessageFormatException {
        return (String) convert(properties.get(name), String.class);
    }

    @Override
    public void setStringProperty(String name, String value) throws JMSException {
        setObjectProperty(name, value);
    }
}
