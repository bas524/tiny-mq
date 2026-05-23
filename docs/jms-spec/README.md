# tiny-mq JMS-spec gap closure

This directory tracks features from the JMS 2.0 specification that are **not yet implemented** in tiny-mq. Every file follows [`_template.md`](./_template.md).

Status legend: `planned` = spec only · `in-progress` = code landing · `shipped` = merged with tests.

## Reading order

Start with [`50-protocol-choice.md`](./50-protocol-choice.md) for the wire-protocol recommendation, then follow the phases below.

## Phase A — Message model foundation

| # | Spec | Status |
|---|---|---|
| 10 | [Message headers](./10-message-headers.md) | planned |
| 11 | [ObjectMessage](./11-object-message.md) | planned |
| 12 | [Per-send DeliveryMode / Priority / TTL](./12-delivery-mode-priority-ttl.md) | planned |
| 13 | [Delivery delay (JMS 2.0)](./13-delivery-delay.md) | planned |
| 14 | [Disable MessageID / Timestamp](./14-disable-msgid-timestamp.md) | planned |
| 44 | [Message expiration sweep](./44-message-expiration-sweep.md) | planned |
| 45 | [Priority ordering on dequeue](./45-priority-ordering.md) | planned |

## Phase B — Session / Consumer semantics

| # | Spec | Status |
|---|---|---|
| 20 | [MessageListener (async push delivery)](./20-message-listener.md) | planned |
| 22 | [DUPS_OK_ACKNOWLEDGE](./22-dups-ok-acknowledge.md) | planned |
| 23 | [Session.recover()](./23-recover.md) | planned |
| 24 | [Redelivery counter + DLQ](./24-redelivery-and-dlq.md) | planned |
| 25 | [NoLocal](./25-no-local.md) | planned |
| 28 | [receiveNoWait](./28-receive-no-wait.md) | planned |
| 33 | [Mixed ack modes per session](./33-mixed-ack-modes.md) | planned |

## Phase C — Advanced consumer features

| # | Spec | Status |
|---|---|---|
| 26 | [Shared consumers (JMS 2.0)](./26-shared-consumers.md) | planned |
| 27 | [QueueBrowser](./27-queue-browser.md) | planned |
| 30 | [Selector grammar audit](./30-selector-audit.md) | planned |
| 31 | [Request/reply pattern](./31-request-reply.md) | planned |

## Phase D — Connection layer & transactions

| # | Spec | Status |
|---|---|---|
| 01 | [Connection / ConnectionFactory](./01-connection-layer.md) | planned |
| 02 | [JMSContext (JMS 2.0 simplified API)](./02-jmscontext.md) | planned |
| 03 | [Client ID, lifecycle, ExceptionListener](./03-client-id-and-lifecycle.md) | planned |
| 21 | [CompletionListener (async send)](./21-completion-listener.md) | planned |
| 32 | [XA transactions](./32-xa-transactions.md) | planned |

## Phase E — Wire protocol & broker

| # | Spec | Status |
|---|---|---|
| 40 | [Wire protocol](./40-wire-protocol.md) | planned |
| 41 | [AuthN / AuthZ / TLS](./41-authn-authz-tls.md) | planned |
| 42 | [Flow control](./42-flow-control.md) | planned |
| 43 | [Admin / introspection](./43-admin-introspection.md) | planned |
| 50 | [Protocol choice recommendation](./50-protocol-choice.md) | decided |

## Phase F — Client libraries

| # | Spec | Status |
|---|---|---|
| 60 | [C++ client library](./60-cpp-client.md) | planned |
| 61 | [Java client library (JMS 2.0 provider)](./61-java-client.md) | planned |
