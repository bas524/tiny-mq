# Java client library (JMS 2.0 provider)

## Purpose
Ship a Java client that implements the `jakarta.jms` (JMS 2.0 / Jakarta Messaging 3.x) interfaces against tiny-mq's wire protocol. This is what makes tiny-mq a "JMS provider" in the standard sense — existing Java apps plug in with zero code changes.

## Scope
- Implement the **standard** JMS interfaces, not a bespoke API. Users import `jakarta.jms.*` and obtain a `ConnectionFactory` from tiny-mq.
- v1 transport: STOMP 1.2 (spec 40). v2 transport: AMQP 1.0 — but at that point it is often cheaper to reuse Qpid JMS instead of maintaining our own Java codec. Decide at v2.

## Directory layout
```
client-java/
├── build.gradle.kts
├── src/main/java/com/tinymq/jms/
│   ├── TinyMqConnectionFactory.java
│   ├── TinyMqConnection.java
│   ├── TinyMqSession.java
│   ├── TinyMqMessageProducer.java
│   ├── TinyMqMessageConsumer.java
│   ├── TinyMqQueueBrowser.java
│   ├── message/
│   │   ├── TinyMqTextMessage.java
│   │   ├── TinyMqBytesMessage.java
│   │   ├── TinyMqStreamMessage.java
│   │   ├── TinyMqMapMessage.java
│   │   └── TinyMqObjectMessage.java
│   ├── wire/
│   │   ├── StompCodec.java
│   │   └── Frame.java
│   └── selector/
│       └── (optional client-side fallback if broker selectors unavailable)
└── src/test/java/com/tinymq/jms/
```

## Proposed API (standard)
```java
ConnectionFactory cf = new TinyMqConnectionFactory("tcp://broker:61613");
try (JMSContext ctx = cf.createContext("user", "pass", JMSContext.AUTO_ACKNOWLEDGE)) {
  Queue q = ctx.createQueue("orders");
  ctx.createProducer().setPriority(7).send(q, "hello");
}
```
Also the legacy API: `createConnection`, `createSession`, `createProducer`, `createConsumer`, `MessageListener`, `QueueBrowser`.

## Interfaces to implement
From `jakarta.jms`:
- `ConnectionFactory`, `Connection`, `Session`, `JMSContext`
- `MessageProducer`, `MessageConsumer`, `QueueBrowser`
- `Queue`, `Topic`, `TemporaryQueue`, `TemporaryTopic`, `Destination`
- `Message`, `TextMessage`, `BytesMessage`, `StreamMessage`, `MapMessage`, `ObjectMessage`
- `MessageListener`, `CompletionListener`, `ExceptionListener`

## Wire mapping
Same as spec 40 (STOMP 1.2). Header name translation:
- `JMSMessageID` ↔ `message-id`
- `JMSTimestamp` ↔ `timestamp`
- `JMSExpiration` ↔ `expires`
- `JMSPriority` ↔ `priority`
- `JMSDeliveryMode` ↔ `persistent`
- `JMSType` ↔ `type`
- `JMSReplyTo` ↔ `reply-to`
- `JMSCorrelationID` ↔ `correlation-id`
- `JMSXDeliveryCount` ↔ `x-delivery-count`
- `JMSRedelivered` ↔ `redelivered`
- `JMSDeliveryTime` ↔ `x-delivery-time`

Body framing by `content-type`:
- `text/plain` → `TextMessage`
- `application/octet-stream` → `BytesMessage`
- `application/x-tinymq-stream` → `StreamMessage`
- `application/x-tinymq-map` → `MapMessage`
- `application/x-tinymq-object;class=Foo.Bar` → `ObjectMessage`

## Selector behaviour
Selectors are evaluated on the broker side (JMS requires this for efficiency). The client sends the expression string verbatim in the SUBSCRIBE `selector` header. The broker must reject malformed selectors with an error frame.

## Threading
- One reader thread per `Connection` (matches JMS contract).
- Session serial-execution guarantee: a session dispatches one `MessageListener.onMessage` at a time, even with multiple consumers — the session owns a single-threaded executor.

## Conformance
Target: pass the **Jakarta Messaging TCK** subset that applies to the implemented features. TCK gaps (e.g. XA if spec 32 is deferred) are documented as non-goals.

## Dependencies
- 10, 01, 03, 40 are hard prerequisites.
- 20 (MessageListener), 21 (CompletionListener), 27 (QueueBrowser), 30 (Selector audit) define what the Java side can expose.

## Test plan
- Unit tests for each interface (ack modes, transactions, durable subs, temp destinations).
- Integration: boot the C++ broker in a Testcontainers image; run JMS TCK-derived test suite against it.
- Interop: Java producer → C++ consumer and vice versa, across all message types and all headers.

## Packaging
- Publish to Maven Central as `com.tinymq:tinymq-jms-client:<version>`.
- Java 17 baseline. Target `jakarta.jms:jakarta.jms-api:3.1.0`.

## Open questions
- Build a real Spring Boot starter (`tinymq-spring-boot-starter`) once the client is stable? Yes — but out of scope for the client spec itself.
- Provide a `javax.jms` (pre-Jakarta) shim? Only on user demand.
