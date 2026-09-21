# connection

## ADDED Requirements

### Requirement: JMSX property names advertised

`ConnectionMetaData::jmsxPropertyNames` SHALL contain exactly `JMSXDeliveryCount` and
`JMSXDeadLetterReason` — the JMS-defined delivery count property (JMS 2.0 § 3.5.9) and the
provider-specific dead-letter reason property introduced by spec 24.

#### Scenario: JmsxPropertyNamesListed

- **WHEN** `ConnectionMetaData::jmsxPropertyNames` is read
- **THEN** it contains exactly `JMSXDeliveryCount` and `JMSXDeadLetterReason`
- Test: `DlqTest.JmsxPropertyNamesListed`
