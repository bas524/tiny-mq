package tinymq.jms;

import jakarta.jms.Destination;
import jakarta.jms.MessageFormatException;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Detached typed values shared with the transport adapter. Array values are copied on ingress and egress. */
public final class MessageEnvelope {
    public enum Kind { MESSAGE, TEXT, BYTES, MAP, STREAM, OBJECT }

    public final Kind kind;
    public final String messageId;
    public final long timestamp;
    public final Destination replyTo;
    public final Destination destination;
    public final int deliveryMode;
    public final String type;
    public final long expiration;
    public final long deliveryTime;
    public final int priority;
    public final String objectClassName;
    private final Object correlationId;
    private final Object body;
    private final Map<String, Object> properties;

    public MessageEnvelope(Kind kind, String messageId, long timestamp, Object correlationId,
                           Destination replyTo, Destination destination, int deliveryMode, String type,
                           long expiration, long deliveryTime, int priority, Map<String, Object> properties,
                           Object body, String objectClassName) throws MessageFormatException {
        this.kind = Objects.requireNonNull(kind);
        this.messageId = messageId;
        this.timestamp = timestamp;
        if (correlationId != null && !(correlationId instanceof String) && !(correlationId instanceof byte[])) {
            throw new MessageFormatException("Invalid correlation ID type");
        }
        this.correlationId = TinyMessage.copyValue(correlationId);
        this.replyTo = replyTo;
        this.destination = destination;
        this.deliveryMode = deliveryMode;
        this.type = type;
        this.expiration = expiration;
        this.deliveryTime = deliveryTime;
        this.priority = priority;
        this.objectClassName = objectClassName;
        this.properties = copyMap(properties, false);
        this.body = copyBody(kind, body);
    }

    public Object correlationId() { return TinyMessage.copyValue(correlationId); }

    public Map<String, Object> properties() { return Collections.unmodifiableMap(properties); }

    public Object body() {
        try {
            return copyBody(kind, body);
        } catch (MessageFormatException impossible) {
            throw new AssertionError(impossible);
        }
    }

    Object bodyValue() { return body; }

    Object correlationValue() { return correlationId; }

    private static Map<String, Object> copyMap(Map<?, ?> values, boolean body) throws MessageFormatException {
        Map<String, Object> copy = new LinkedHashMap<>();
        for (Map.Entry<?, ?> entry : values.entrySet()) {
            if (!(entry.getKey() instanceof String) || ((String) entry.getKey()).isEmpty()
                    || (!body && !TinyMessage.validPropertyName((String) entry.getKey()))) {
                throw new MessageFormatException("Invalid " + (body ? "map key" : "property name"));
            }
            TinyMessage.checkValue(entry.getValue(), body);
            copy.put((String) entry.getKey(), TinyMessage.copyValue(entry.getValue()));
        }
        return copy;
    }

    private static Object copyBody(Kind kind, Object value) throws MessageFormatException {
        switch (kind) {
            case MESSAGE:
                if (value == null) return null;
                break;
            case TEXT:
                if (value == null || value instanceof String) return value;
                break;
            case BYTES:
                if (value instanceof byte[]) return TinyMessage.copyValue(value);
                break;
            case OBJECT:
                if (value == null || value instanceof byte[]) return TinyMessage.copyValue(value);
                break;
            case MAP:
                if (value instanceof Map<?, ?>) return copyMap((Map<?, ?>) value, true);
                break;
            case STREAM:
                if (value instanceof List<?>) {
                    List<Object> copy = new ArrayList<>();
                    for (Object element : (List<?>) value) {
                        TinyMessage.checkValue(element, true);
                        copy.add(TinyMessage.copyValue(element));
                    }
                    return copy;
                }
                break;
            default:
                throw new AssertionError(kind);
        }
        throw new MessageFormatException("Invalid body for " + kind);
    }
}
