package tinymq.jms;

import jakarta.jms.JMSException;
import jakarta.jms.MapMessage;
import jakarta.jms.MessageFormatException;

import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

public final class TinyMapMessage extends TinyMessage implements MapMessage {
    private final Map<String, Object> values;

    public TinyMapMessage() { values = new LinkedHashMap<>(); }

    TinyMapMessage(Map<String, Object> source) { values = copy(source); }

    @Override
    public Enumeration<String> getMapNames() { return Collections.enumeration(values.keySet()); }

    @Override
    public boolean itemExists(String name) { return values.containsKey(name); }

    @Override
    public Object getObject(String name) { return copyValue(values.get(name)); }

    @Override
    public void setObject(String name, Object value) throws JMSException {
        requireBodyWriteable();
        if (name == null || name.isEmpty()) throw new IllegalArgumentException("Empty map key");
        checkValue(value, true);
        values.put(name, copyValue(value));
    }

    @Override
    public void setBytes(String name, byte[] value, int offset, int length) throws JMSException {
        requireBodyWriteable();
        Objects.checkFromIndexSize(offset, length, value.length);
        byte[] slice = new byte[length];
        System.arraycopy(value, offset, slice, 0, length);
        setObject(name, slice);
    }

    private static Map<String, Object> copy(Map<String, Object> source) {
        Map<String, Object> result = new LinkedHashMap<>();
        source.forEach((key, value) -> result.put(key, copyValue(value)));
        return result;
    }

    @Override
    Object snapshotBody() { return values; }

    @Override
    Object bodyValue() { return values.isEmpty() ? null : copy(values); }

    @Override
    public <T> T getBody(Class<T> requested) throws JMSException {
        if (!isBodyAssignableTo(requested)) throw new MessageFormatException("Incompatible body type");
        return super.getBody(requested);
    }

    @Override
    public boolean isBodyAssignableTo(Class requested) {
        return values.isEmpty() || requested == Map.class || requested == Object.class;
    }

    @Override
    void discardBody() { values.clear(); }

    @Override
    public boolean getBoolean(String name) throws JMSException {
        return (Boolean) convert(values.get(name), Boolean.class);
    }

    @Override
    public void setBoolean(String name, boolean value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public byte getByte(String name) throws JMSException {
        return (Byte) convert(values.get(name), Byte.class);
    }

    @Override
    public void setByte(String name, byte value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public short getShort(String name) throws JMSException {
        return (Short) convert(values.get(name), Short.class);
    }

    @Override
    public void setShort(String name, short value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public char getChar(String name) throws JMSException {
        return (Character) convert(values.get(name), Character.class);
    }

    @Override
    public void setChar(String name, char value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public int getInt(String name) throws JMSException {
        return (Integer) convert(values.get(name), Integer.class);
    }

    @Override
    public void setInt(String name, int value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public long getLong(String name) throws JMSException {
        return (Long) convert(values.get(name), Long.class);
    }

    @Override
    public void setLong(String name, long value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public float getFloat(String name) throws JMSException {
        return (Float) convert(values.get(name), Float.class);
    }

    @Override
    public void setFloat(String name, float value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public double getDouble(String name) throws JMSException {
        return (Double) convert(values.get(name), Double.class);
    }

    @Override
    public void setDouble(String name, double value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public String getString(String name) throws JMSException {
        return (String) convert(values.get(name), String.class);
    }

    @Override
    public void setString(String name, String value) throws JMSException {
        setObject(name, value);
    }

    @Override
    public byte[] getBytes(String name) throws JMSException {
        return (byte[]) convert(values.get(name), byte[].class);
    }

    @Override
    public void setBytes(String name, byte[] value) throws JMSException {
        setObject(name, value);
    }
}
