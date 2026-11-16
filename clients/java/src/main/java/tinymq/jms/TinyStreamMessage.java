package tinymq.jms;

import jakarta.jms.JMSException;
import jakarta.jms.MessageEOFException;
import jakarta.jms.MessageFormatException;
import jakarta.jms.MessageNotReadableException;
import jakarta.jms.StreamMessage;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

public final class TinyStreamMessage extends TinyMessage implements StreamMessage {
    private final List<Object> values = new ArrayList<>();
    private int position;
    private int arrayOffset = -1;

    public TinyStreamMessage() {}

    TinyStreamMessage(List<Object> source) {
        source.forEach(value -> values.add(copyValue(value)));
    }

    private Object peek() throws JMSException {
        if (!bodyReadOnly()) throw new MessageNotReadableException("Stream body is write-only");
        if (position == values.size()) throw new MessageEOFException("End of stream body");
        return values.get(position);
    }

    private Object read(Class<?> requested) throws JMSException {
        Object value = peek();
        if (arrayOffset >= 0) throw new MessageFormatException("Finish reading the byte array first");
        Object result = convert(value, requested);
        ++position;
        return result;
    }

    @Override
    public Object readObject() throws JMSException { return read(Object.class); }

    @Override
    public int readBytes(byte[] value) throws JMSException {
        Object element = peek();
        Objects.requireNonNull(value);
        if (element == null) {
            ++position;
            return -1;
        }
        if (!(element instanceof byte[])) throw new MessageFormatException("Stream item is not a byte array");
        byte[] data = (byte[]) element;
        if (arrayOffset == data.length) {
            arrayOffset = -1;
            ++position;
            return -1;
        }
        int start = Math.max(arrayOffset, 0);
        int length = Math.min(data.length - start, value.length);
        System.arraycopy(data, start, value, 0, length);
        if (length < value.length) {
            arrayOffset = -1;
            ++position;
        } else {
            // A read that fills the caller's buffer requires one more readBytes.
            arrayOffset = start + length;
        }
        return length;
    }

    @Override
    public void writeObject(Object value) throws JMSException {
        requireBodyWriteable();
        checkValue(value, true);
        values.add(copyValue(value));
    }

    @Override
    public void writeBytes(byte[] value, int offset, int length) throws JMSException {
        requireBodyWriteable();
        Objects.checkFromIndexSize(offset, length, value.length);
        byte[] slice = new byte[length];
        System.arraycopy(value, offset, slice, 0, length);
        values.add(slice);
    }

    @Override
    public void reset() { makeBodyReadable(); }

    @Override
    void makeBodyReadable() {
        position = 0;
        arrayOffset = -1;
        super.makeBodyReadable();
    }

    @Override
    public <T> T getBody(Class<T> requested) throws MessageFormatException {
        throw new MessageFormatException("StreamMessage exposes its body through sequential reads");
    }

    @Override
    public boolean isBodyAssignableTo(Class requested) { return false; }

    @Override
    Object snapshotBody() { return values; }

    @Override
    void discardBody() {
        values.clear();
        position = 0;
        arrayOffset = -1;
    }

    @Override
    public boolean readBoolean() throws JMSException { return (Boolean) read(Boolean.class); }

    @Override
    public void writeBoolean(boolean value) throws JMSException { writeObject(value); }

    @Override
    public byte readByte() throws JMSException { return (Byte) read(Byte.class); }

    @Override
    public void writeByte(byte value) throws JMSException { writeObject(value); }

    @Override
    public short readShort() throws JMSException { return (Short) read(Short.class); }

    @Override
    public void writeShort(short value) throws JMSException { writeObject(value); }

    @Override
    public char readChar() throws JMSException { return (Character) read(Character.class); }

    @Override
    public void writeChar(char value) throws JMSException { writeObject(value); }

    @Override
    public int readInt() throws JMSException { return (Integer) read(Integer.class); }

    @Override
    public void writeInt(int value) throws JMSException { writeObject(value); }

    @Override
    public long readLong() throws JMSException { return (Long) read(Long.class); }

    @Override
    public void writeLong(long value) throws JMSException { writeObject(value); }

    @Override
    public float readFloat() throws JMSException { return (Float) read(Float.class); }

    @Override
    public void writeFloat(float value) throws JMSException { writeObject(value); }

    @Override
    public double readDouble() throws JMSException { return (Double) read(Double.class); }

    @Override
    public void writeDouble(double value) throws JMSException { writeObject(value); }

    @Override
    public String readString() throws JMSException { return (String) read(String.class); }

    @Override
    public void writeString(String value) throws JMSException { writeObject(value); }

    @Override
    public void writeBytes(byte[] value) throws JMSException { writeObject(value); }
}
