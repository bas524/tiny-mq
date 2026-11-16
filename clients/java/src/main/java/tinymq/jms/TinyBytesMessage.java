package tinymq.jms;

import jakarta.jms.BytesMessage;
import jakarta.jms.JMSException;
import jakarta.jms.MessageEOFException;
import jakarta.jms.MessageFormatException;
import jakarta.jms.MessageNotReadableException;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.util.Objects;

public final class TinyBytesMessage extends TinyMessage implements BytesMessage {
    private ByteArrayOutputStream buffer = new ByteArrayOutputStream();
    private byte[] bytes;
    private ByteArrayInputStream cursor;
    private DataInputStream input;

    public TinyBytesMessage() {}

    TinyBytesMessage(byte[] data) { bytes = data.clone(); }

    @Override
    public void reset() {
        makeBodyReadable();
    }

    @Override
    void makeBodyReadable() {
        if (bytes == null) bytes = buffer.toByteArray();
        cursor = new ByteArrayInputStream(bytes);
        input = new DataInputStream(cursor);
        super.makeBodyReadable();
    }

    private void requireReadable() throws MessageNotReadableException {
        if (!bodyReadOnly()) throw new MessageNotReadableException("Bytes body is write-only");
    }

    private interface Read<T> { T read(DataInputStream data) throws IOException; }

    private <T> T read(Read<T> action) throws JMSException {
        requireReadable();
        cursor.mark(0);
        try {
            return action.read(input);
        } catch (EOFException failure) {
            cursor.reset();
            throw new MessageEOFException("End of bytes body");
        } catch (IOException failure) {
            cursor.reset();
            MessageFormatException error = new MessageFormatException("Malformed bytes body");
            error.setLinkedException(failure);
            error.initCause(failure);
            throw error;
        }
    }

    private interface Write { void write(DataOutputStream data) throws IOException; }

    private void write(Write action) throws JMSException {
        requireBodyWriteable();
        try {
            action.write(new DataOutputStream(buffer));
        } catch (IOException failure) {
            MessageFormatException error = new MessageFormatException("Cannot write bytes body");
            error.setLinkedException(failure);
            error.initCause(failure);
            throw error;
        }
    }

    @Override
    public long getBodyLength() throws JMSException {
        requireReadable();
        return bytes.length;
    }

    @Override
    public int readBytes(byte[] value) throws JMSException {
        return readBytes(value, value.length);
    }

    @Override
    public int readBytes(byte[] value, int length) throws JMSException {
        requireReadable();
        Objects.checkFromIndexSize(0, length, value.length);
        return length == 0 ? 0 : cursor.read(value, 0, length);
    }

    @Override
    public void writeBytes(byte[] value) throws JMSException {
        writeBytes(value, 0, value.length);
    }

    @Override
    public void writeBytes(byte[] value, int offset, int length) throws JMSException {
        requireBodyWriteable();
        buffer.write(value, offset, length);
    }

    @Override
    public void writeObject(Object value) throws JMSException {
        requireBodyWriteable();
        Objects.requireNonNull(value);
        if (value instanceof Boolean) writeBoolean((Boolean) value);
        else if (value instanceof Byte) writeByte((Byte) value);
        else if (value instanceof Short) writeShort((Short) value);
        else if (value instanceof Character) writeChar((Character) value);
        else if (value instanceof Integer) writeInt((Integer) value);
        else if (value instanceof Long) writeLong((Long) value);
        else if (value instanceof Float) writeFloat((Float) value);
        else if (value instanceof Double) writeDouble((Double) value);
        else if (value instanceof String) writeUTF((String) value);
        else if (value instanceof byte[]) writeBytes((byte[]) value);
        else throw new MessageFormatException("Unsupported bytes body value: " + value.getClass().getTypeName());
    }

    @Override
    Object snapshotBody() { return bytes == null ? buffer.toByteArray() : bytes; }

    @Override
    Object bodyValue() {
        reset();
        return bytes.length == 0 ? null : bytes.clone();
    }

    @Override
    public <T> T getBody(Class<T> requested) throws JMSException {
        reset();
        if (!isBodyAssignableTo(requested)) throw new MessageFormatException("Incompatible body type");
        return super.getBody(requested);
    }

    @Override
    public boolean isBodyAssignableTo(Class requested) {
        return (bodyReadOnly() ? bytes.length == 0 : buffer.size() == 0) || requested == byte[].class || requested == Object.class;
    }

    @Override
    void discardBody() {
        buffer = new ByteArrayOutputStream();
        bytes = null;
        cursor = null;
        input = null;
    }

    @Override
    public boolean readBoolean() throws JMSException { return read(DataInputStream::readBoolean); }

    @Override
    public byte readByte() throws JMSException { return read(DataInputStream::readByte); }

    @Override
    public short readShort() throws JMSException { return read(DataInputStream::readShort); }

    @Override
    public char readChar() throws JMSException { return read(DataInputStream::readChar); }

    @Override
    public int readInt() throws JMSException { return read(DataInputStream::readInt); }

    @Override
    public long readLong() throws JMSException { return read(DataInputStream::readLong); }

    @Override
    public float readFloat() throws JMSException { return read(DataInputStream::readFloat); }

    @Override
    public double readDouble() throws JMSException { return read(DataInputStream::readDouble); }

    @Override
    public int readUnsignedByte() throws JMSException { return read(DataInputStream::readUnsignedByte); }

    @Override
    public int readUnsignedShort() throws JMSException { return read(DataInputStream::readUnsignedShort); }

    @Override
    public String readUTF() throws JMSException { return read(data -> data.readUTF()); }

    @Override
    public void writeBoolean(boolean value) throws JMSException { write(data -> data.writeBoolean(value)); }

    @Override
    public void writeByte(byte value) throws JMSException { write(data -> data.writeByte(value)); }

    @Override
    public void writeShort(short value) throws JMSException { write(data -> data.writeShort(value)); }

    @Override
    public void writeChar(char value) throws JMSException { write(data -> data.writeChar(value)); }

    @Override
    public void writeInt(int value) throws JMSException { write(data -> data.writeInt(value)); }

    @Override
    public void writeLong(long value) throws JMSException { write(data -> data.writeLong(value)); }

    @Override
    public void writeFloat(float value) throws JMSException { write(data -> data.writeFloat(value)); }

    @Override
    public void writeDouble(double value) throws JMSException { write(data -> data.writeDouble(value)); }

    @Override
    public void writeUTF(String value) throws JMSException { write(data -> data.writeUTF(value)); }
}
