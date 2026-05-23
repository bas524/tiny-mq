# Shared consumers (JMS 2.0)

## JMS reference
- JMS 2.0 § 8.3 `createSharedConsumer`, `createSharedDurableConsumer`.

## Current state in tiny-mq
- Durable topic subs are per-consumer only: `DurableSubState::activeConsumerUuid` is a single UUID (`Destination.h:21`), with a comment "null UUID = offline".
- No shared (non-durable) consumer concept.

## Proposed API
```cpp
Consumer::Ptr Session::createSharedConsumer(const Destination::Ptr& topic,
                                            const std::string& sharedSubName,
                                            const std::string& selector = "");
Consumer::Ptr Session::createSharedDurableConsumer(const Destination::Ptr& topic,
                                                   const std::string& name,
                                                   const std::string& selector = "");
```

## Semantics
- Each message published to the topic is delivered **once** across the set of consumers sharing a subscription — effectively a per-subscription work queue on top of a topic.
- `Destination` fan-out for topics must change: for each subscription, pick one consumer (round-robin or load-aware) instead of broadcasting.

## Persistence / wire implications
- Durable variant: subscription storage is shared; multiple consumer UUIDs may pop from the same `ConcurrentLinearStorage`.
- `DurableSubState` must hold a set of active consumer UUIDs rather than a single one.

## Dependencies
- 10 (headers), 25 (NoLocal semantics clarified in shared context).

## Test plan
- `SharedConsumerTest` (non-durable): 3 consumers sharing a subscription; each message received by exactly one.
- `SharedDurableConsumerTest`: consumer disconnect does not lose messages if another shared consumer is online.

## Open questions
- Load-balancing policy: round-robin vs "least-pending"? Start with round-robin.
