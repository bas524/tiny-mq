package tinymq.jms;

import jakarta.jms.JMSException;
import jakarta.jms.MessageFormatException;
import jakarta.jms.ObjectMessage;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.io.Serializable;

public final class TinyObjectMessage extends TinyMessage implements ObjectMessage {
    private byte[] serialized;
    private String className;

    public TinyObjectMessage() {}

    public TinyObjectMessage(Serializable value) throws JMSException {
        setObject(value);
    }

    TinyObjectMessage(byte[] data, String className) {
        serialized = data == null ? null : data.clone();
        this.className = className;
    }

    @Override
    public void setObject(Serializable value) throws JMSException {
        requireBodyWriteable();
        if (value == null) {
            discardBody();
            return;
        }
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        try (ObjectOutputStream output = new ObjectOutputStream(buffer)) {
            output.writeObject(value);
        } catch (IOException | RuntimeException failure) {
            throw formatFailure("Cannot serialize object body", failure);
        }
        serialized = buffer.toByteArray();
        className = null;
    }

    @Override
    public Serializable getObject() throws MessageFormatException {
        if (serialized == null) return null;
        try (ObjectInputStream input = new ObjectInputStream(new ByteArrayInputStream(serialized))) {
            return (Serializable) input.readObject();
        } catch (IOException | ClassNotFoundException | RuntimeException failure) {
            throw formatFailure("Cannot deserialize object body", failure);
        }
    }

    private static MessageFormatException formatFailure(String message, Exception cause) {
        MessageFormatException error = new MessageFormatException(message);
        error.setLinkedException(cause);
        error.initCause(cause);
        return error;
    }

    String className() { return className; }

    @Override
    Object snapshotBody() { return serialized; }

    @Override
    Object bodyValue() throws MessageFormatException { return getObject(); }

    @Override
    void discardBody() {
        serialized = null;
        className = null;
    }
}
