package tinymq.jms;

import jakarta.jms.Destination;
import jakarta.jms.JMSException;
import jakarta.jms.MessageFormatException;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

public final class JavaMessageAdapter {
    private JavaMessageAdapter() {}

    @FunctionalInterface
    public interface Acknowledgement {
        void acknowledge() throws JMSException;
    }

    /** The provider supplies computed send values, including the ID and timestamp hint results. */
    public static MessageEnvelope snapshotForSend(TinyMessage message, String messageId, long timestamp,
                                                  Destination destination, int deliveryMode, int priority,
                                                  long expiration, long deliveryTime) throws MessageFormatException {
        MessageEnvelope.Kind kind = MessageEnvelope.Kind.MESSAGE;
        String objectClassName = null;
        if (message instanceof TinyTextMessage) kind = MessageEnvelope.Kind.TEXT;
        else if (message instanceof TinyBytesMessage) kind = MessageEnvelope.Kind.BYTES;
        else if (message instanceof TinyMapMessage) kind = MessageEnvelope.Kind.MAP;
        else if (message instanceof TinyStreamMessage) kind = MessageEnvelope.Kind.STREAM;
        else if (message instanceof TinyObjectMessage) {
            kind = MessageEnvelope.Kind.OBJECT;
            objectClassName = ((TinyObjectMessage) message).className();
        }
        Map<String, Object> properties = new LinkedHashMap<>(message.properties);
        properties.remove("JMSXDeliveryCount");
        MessageEnvelope envelope = new MessageEnvelope(kind, messageId, timestamp, message.correlationValue(),
                message.getJMSReplyTo(), destination, deliveryMode, message.getJMSType(), expiration, deliveryTime,
                priority, properties, message.snapshotBody(), objectClassName);
        applySendHeaders(message, envelope);
        return envelope;
    }

    /** Each delivery gets a fresh view of the original frozen envelope and its attempt's metadata. */
    @SuppressWarnings("unchecked")
    public static TinyMessage forDelivery(MessageEnvelope envelope, boolean redelivered, int deliveryCount,
                                          Acknowledgement acknowledgement) {
        Objects.requireNonNull(envelope);
        if (deliveryCount < (redelivered ? 2 : 1)) {
            throw new IllegalArgumentException("Inconsistent delivery count and redelivery flag");
        }
        Object body = envelope.bodyValue();
        TinyMessage message;
        switch (envelope.kind) {
            case MESSAGE: message = new TinyMessage(); break;
            case TEXT: message = new TinyTextMessage((String) body); break;
            case BYTES: message = new TinyBytesMessage((byte[]) body); break;
            case MAP: message = new TinyMapMessage((Map<String, Object>) body); break;
            case STREAM: message = new TinyStreamMessage((List<Object>) body); break;
            case OBJECT: message = new TinyObjectMessage((byte[]) body, envelope.objectClassName); break;
            default: throw new AssertionError(envelope.kind);
        }
        applySendHeaders(message, envelope);
        message.restoreCorrelation(envelope.correlationValue());
        message.setJMSReplyTo(envelope.replyTo);
        message.setJMSType(envelope.type);
        message.setJMSRedelivered(redelivered);
        message.properties.putAll(envelope.properties());
        message.properties.put("JMSXDeliveryCount", deliveryCount);
        message.makeReceived(acknowledgement);
        return message;
    }

    private static void applySendHeaders(TinyMessage message, MessageEnvelope envelope) {
        message.setJMSMessageID(envelope.messageId);
        message.setJMSTimestamp(envelope.timestamp);
        message.setJMSDestination(envelope.destination);
        message.setJMSDeliveryMode(envelope.deliveryMode);
        message.setJMSPriority(envelope.priority);
        message.setJMSExpiration(envelope.expiration);
        message.setJMSDeliveryTime(envelope.deliveryTime);
    }
}
