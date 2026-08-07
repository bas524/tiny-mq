//
// Session.recover() tests for tiny-mq (JMS 2.0 § 8.4.8, spec 23)
//

#include "RecoverTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "Exceptions.h"
#include "TestHelper.h"

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::IllegalStateException;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::TextMessage;

void RecoverTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void RecoverTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

// Test plan (spec 23): CLIENT_ACK — receive 3, ack 1, recover, expect the other
// 2 redelivered with redelivered=true, in original order. Persistent messages,
// so a cache/header desync (ADR-0008) would show up as a lost redelivered flag
// on the second recv().
TEST_F(RecoverTest, ClientAckRedeliversUnacked) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m0 = session.createTextMessage("first", Message::PERSISTENT);
  TextMessage m1 = session.createTextMessage("second", Message::PERSISTENT);
  TextMessage m2 = session.createTextMessage("third", Message::PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));
  EXPECT_NO_THROW(producer->send(m1));
  EXPECT_NO_THROW(producer->send(m2));

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumer->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(consumer->recv());
  TextMessage::Ptr r2 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ("first", r0->text());
  EXPECT_EQ("second", r1->text());
  EXPECT_EQ("third", r2->text());
  // Nothing has been redelivered yet.
  EXPECT_FALSE(r0->jmsHeaders.redelivered);
  EXPECT_FALSE(r1->jmsHeaders.redelivered);
  EXPECT_FALSE(r2->jmsHeaders.redelivered);
  EXPECT_EQ(0, r0->jmsHeaders.deliveryCount);
  EXPECT_EQ(0, r1->jmsHeaders.deliveryCount);
  EXPECT_EQ(0, r2->jmsHeaders.deliveryCount);

  // Ack only the first message — the other two remain in-flight.
  EXPECT_NO_THROW(consumer->acknowledgeOn(*r0));

  EXPECT_NO_THROW(session.recover());

  // The two unacknowledged messages come back, in original order, marked
  // redelivered with deliveryCount incremented. The acknowledged message
  // must not reappear.
  TextMessage::Ptr again0 = Message::As<TextMessage>(consumer->recv());
  TextMessage::Ptr again1 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(again0, nullptr);
  ASSERT_NE(again1, nullptr);
  EXPECT_EQ("second", again0->text());
  EXPECT_EQ("third", again1->text());
  EXPECT_TRUE(again0->jmsHeaders.redelivered);
  EXPECT_TRUE(again1->jmsHeaders.redelivered);
  EXPECT_EQ(1, again0->jmsHeaders.deliveryCount);
  EXPECT_EQ(1, again1->jmsHeaders.deliveryCount);

  // No further messages are pending.
  TextMessage::Ptr none = Message::As<TextMessage>(consumer->recv(50000));
  EXPECT_EQ(none, nullptr);

  EXPECT_NO_THROW(consumer->acknowledgeOn(*again0));
  EXPECT_NO_THROW(consumer->acknowledgeOn(*again1));
}

// recover() is illegal on a transacted session — the application must call
// rollback() instead (JMS 2.0 § 8.4.8).
TEST_F(RecoverTest, RejectsOnTransactedSession) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m0 = session.createTextMessage("tx-msg", Message::PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));
  session.commit();

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(r0, nullptr);

  EXPECT_THROW(session.recover(), IllegalStateException);
}

// Non-persistent messages: recover() must still requeue and mark them without
// touching storage (there is none to touch).
TEST_F(RecoverTest, NonPersistentRedelivery) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m0 = session.createTextMessage("np", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(r0, nullptr);
  EXPECT_FALSE(r0->jmsHeaders.redelivered);

  EXPECT_NO_THROW(session.recover());

  TextMessage::Ptr again = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(again, nullptr);
  EXPECT_EQ("np", again->text());
  EXPECT_TRUE(again->jmsHeaders.redelivered);
  EXPECT_EQ(1, again->jmsHeaders.deliveryCount);

  EXPECT_NO_THROW(consumer->acknowledgeOn(*again));
}

// Open question (spec 23): redelivered messages on a durable topic subscriber
// must not be re-persisted to durable storage — they are already there from
// their first delivery. Verified by counting deliveries: with no re-persist,
// recover() replays exactly the unacknowledged messages once, never more.
TEST_F(RecoverTest, DurableSubscriberNotRepersisted) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);
  ASSERT_NE(topic, nullptr);

  Consumer::Ptr sub = session.createDurableConsumer(topic, "recover-sub");
  ASSERT_NE(sub, nullptr);
  Producer::Ptr producer = session.createProducer(topic);
  ASSERT_NE(producer, nullptr);

  TextMessage m0 = session.createTextMessage("d0", Message::PERSISTENT);
  TextMessage m1 = session.createTextMessage("d1", Message::PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));
  EXPECT_NO_THROW(producer->send(m1));

  TextMessage::Ptr r0 = Message::As<TextMessage>(sub->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(sub->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);

  EXPECT_NO_THROW(session.recover());

  // Exactly 2 messages replay — no duplicates from a re-persist + replay.
  TextMessage::Ptr again0 = Message::As<TextMessage>(sub->recv());
  TextMessage::Ptr again1 = Message::As<TextMessage>(sub->recv());
  ASSERT_NE(again0, nullptr);
  ASSERT_NE(again1, nullptr);
  EXPECT_TRUE(again0->jmsHeaders.redelivered);
  EXPECT_TRUE(again1->jmsHeaders.redelivered);

  TextMessage::Ptr none = Message::As<TextMessage>(sub->recv(50000));
  EXPECT_EQ(none, nullptr) << "recover() must not duplicate messages via re-persistence to durable storage";

  EXPECT_NO_THROW(sub->acknowledgeOn(*again0));
  EXPECT_NO_THROW(sub->acknowledgeOn(*again1));
  session.unsubscribe(topic, "recover-sub");
}

// --- Spec 23 round 3 (review round 2, B2): regression tests for the two
// intrusive-in-flight-list defects the round-2 perf fix introduced —
// (a) Message::copy() inherited chain-membership pointers from the source,
// and (b) acknowledgeOn() treated "linked into someone's chain" as "linked
// into mine". Both are closed by: a hand-written Message copy/move
// constructor that leaves the intrusive fields at their default (null)
// value (a copy is never itself linked into anything), and an explicit
// _inFlightOwner tag that acknowledgeOn() checks against `this` instead of
// inferring ownership from the prev/head shape of the chain. ---

// ZZ5: acknowledge a message on the wrong consumer of the same queue. Round 1
// (a plain std::vector<Message::Ptr> per consumer, searched by identity) made
// this a harmless no-op — the message simply wasn't found in the wrong
// consumer's own vector. Round 2's intrusive list broke that: any message
// with a non-null prev (i.e. not the current head) looked "tracked" to
// whichever consumer happened to call acknowledgeOn(), regardless of whose
// chain it actually lived in, and spliced it out of the real owner's chain.
TEST_F(RecoverTest, AcknowledgeOnWrongConsumerOfSameQueueIsNoOp) {
  Connection connection(*_exchange);
  // c1 and c2 live in separate sessions so that Session::recover() (which
  // recovers *every* consumer of its session) exercises c1's chain only —
  // isolating the wrong-consumer-ack defect from the (unrelated, and
  // separately legal per spec 23 §7 O3) fact that both consumers still
  // share one underlying queue for delivery.
  Session &sessionC1 = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Session &sessionC2 = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = sessionC1.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = sessionC1.createProducer(queue);
  Consumer::Ptr c1 = sessionC1.createConsumer(queue);
  Consumer::Ptr c2 = sessionC2.createConsumer(queue);  // same queue, different session

  TextMessage m0 = sessionC1.createTextMessage("m0", Message::NOT_PERSISTENT);
  TextMessage m1 = sessionC1.createTextMessage("m1", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));
  EXPECT_NO_THROW(producer->send(m1));

  // c2 hasn't called recv() yet, so both messages land on c1 — deterministic
  // in a single-threaded test. c1's chain is now [r0, r1]; r1 is the tail
  // with a non-null prev.
  TextMessage::Ptr r0 = Message::As<TextMessage>(c1->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(c1->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);

  // Acknowledge r1 on c2 — the wrong consumer. Must be a no-op: r1 stays in
  // c1's chain.
  EXPECT_NO_THROW(c2->acknowledgeOn(*r1));

  TextMessage m2 = sessionC1.createTextMessage("m2", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producer->send(m2));
  TextMessage::Ptr r2 = Message::As<TextMessage>(c2->recv());
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ("m2", r2->text())
      << "m2 must reach c2 normally; a corrupted c2._inFlightTail (aliasing "
         "c1's chain) would misroute or drop it";

  // Only c1's session recovers — c1 still owns both r0 and r1 (the
  // wrong-consumer ack changed nothing); c2's own recv of m2 is untouched.
  EXPECT_NO_THROW(sessionC1.recover());

  TextMessage::Ptr again0 = Message::As<TextMessage>(c1->recv(50000));
  TextMessage::Ptr again1 = Message::As<TextMessage>(c1->recv(50000));
  ASSERT_NE(again0, nullptr);
  ASSERT_NE(again1, nullptr);
  EXPECT_EQ("m0", again0->text());
  EXPECT_EQ("m1", again1->text());
  EXPECT_TRUE(again0->jmsHeaders.redelivered);
  EXPECT_TRUE(again1->jmsHeaders.redelivered);
  TextMessage::Ptr noneFromC1 = Message::As<TextMessage>(c1->recv(50000));
  EXPECT_EQ(noneFromC1, nullptr) << "c2's ack redelivered 0 (expected c1's own 2)";

  // c2's recover() (own session) still returns its own r2 — untouched by
  // the earlier no-op ack.
  EXPECT_NO_THROW(sessionC2.recover());
  TextMessage::Ptr againC2 = Message::As<TextMessage>(c2->recv(50000));
  ASSERT_NE(againC2, nullptr);
  EXPECT_EQ("m2", againC2->text());
  TextMessage::Ptr noneFromC2 = Message::As<TextMessage>(c2->recv(50000));
  EXPECT_EQ(noneFromC2, nullptr);

  EXPECT_NO_THROW(c1->acknowledgeOn(*again0));
  EXPECT_NO_THROW(c1->acknowledgeOn(*again1));
  EXPECT_NO_THROW(c2->acknowledgeOn(*againC2));
}

// ZZ1: forward an in-flight message with a non-null _inFlightNext (i.e. it
// is not the tail of its consumer's chain) to a different consumer on a
// different session/destination — a mainstream JMS pattern (bridge, DLQ,
// retry). A copy() that inherits the source's owning _inFlightNext would
// hand the copy — and through it, the receiving consumer — a live reference
// into the source's own chain, leaking a message the receiver was never
// actually sent.
TEST_F(RecoverTest, ForwardingHeadMessageDoesNotLeakChainToOtherConsumer) {
  Connection connection(*_exchange);
  Session &sessionA = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queueA = sessionA.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + "-A");
  ASSERT_NE(queueA, nullptr);
  Producer::Ptr producerA = sessionA.createProducer(queueA);
  Consumer::Ptr consumerA = sessionA.createConsumer(queueA);

  TextMessage a0 = sessionA.createTextMessage("A-first", Message::NOT_PERSISTENT);
  TextMessage a1 = sessionA.createTextMessage("A-second", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producerA->send(a0));
  EXPECT_NO_THROW(producerA->send(a1));

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumerA->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(consumerA->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  // r0 is the chain head, with r1 as its (owning) next.

  Session &sessionB = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queueB = sessionB.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + "-B");
  ASSERT_NE(queueB, nullptr);
  Producer::Ptr producerB = sessionB.createProducer(queueB);
  Consumer::Ptr consumerB = sessionB.createConsumer(queueB);

  EXPECT_NO_THROW(producerB->send(*r0));  // forward: Producer::send() copies via Message::copy()

  TextMessage::Ptr b0 = Message::As<TextMessage>(consumerB->recv());
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ("A-first", b0->text());

  // Recovering B's own (single-message) chain must return exactly that one
  // message — not additionally walk into whatever the copy's inherited
  // _inFlightNext used to alias on A's side. The old ("tracked" =
  // non-null-prev-or-head) unlink test isn't exercised by recover() itself
  // (it walks the chain unconditionally), so this is where B2(a) — the
  // copy-inherits-links half of the defect — actually surfaces.
  EXPECT_NO_THROW(sessionB.recover());
  TextMessage::Ptr bAgain = Message::As<TextMessage>(consumerB->recv(50000));
  ASSERT_NE(bAgain, nullptr);
  EXPECT_EQ("A-first", bAgain->text());
  TextMessage::Ptr leak = Message::As<TextMessage>(consumerB->recv(50000));
  EXPECT_EQ(leak, nullptr) << "LEAK: consumer B received a message it was never sent";
  EXPECT_NO_THROW(consumerB->acknowledgeOn(*bAgain));

  // A's own chain must be intact — recover() still returns both messages,
  // in order.
  EXPECT_NO_THROW(sessionA.recover());
  TextMessage::Ptr again0 = Message::As<TextMessage>(consumerA->recv(50000));
  TextMessage::Ptr again1 = Message::As<TextMessage>(consumerA->recv(50000));
  ASSERT_NE(again0, nullptr);
  ASSERT_NE(again1, nullptr);
  EXPECT_EQ("A-first", again0->text());
  EXPECT_EQ("A-second", again1->text());

  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again0));
  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again1));
}

// ZZ2: forward a message from the *middle* of a chain, then acknowledge it
// on its original consumer (the "forward, then treat as handled" pattern).
// Exercises the same copy()-shares-pointers defect from the opposite end:
// the forwarded copy and the original message end up with two independent
// _inFlightNext members that (pre-fix) briefly co-owned the same downstream
// node, so unlinking on one side could zombie/lose messages on the other.
TEST_F(RecoverTest, ForwardingMiddleMessageDoesNotCorruptEitherChain) {
  Connection connection(*_exchange);
  Session &sessionA = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queueA = sessionA.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + "-A");
  ASSERT_NE(queueA, nullptr);
  Producer::Ptr producerA = sessionA.createProducer(queueA);
  Consumer::Ptr consumerA = sessionA.createConsumer(queueA);

  TextMessage a0 = sessionA.createTextMessage("A-first", Message::NOT_PERSISTENT);
  TextMessage a1 = sessionA.createTextMessage("A-second", Message::NOT_PERSISTENT);
  TextMessage a2 = sessionA.createTextMessage("A-third", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producerA->send(a0));
  EXPECT_NO_THROW(producerA->send(a1));
  EXPECT_NO_THROW(producerA->send(a2));

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumerA->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(consumerA->recv());
  TextMessage::Ptr r2 = Message::As<TextMessage>(consumerA->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  // Chain: r0 -> r1 -> r2. r1 is the middle node.

  Session &sessionB = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queueB = sessionB.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + "-B");
  ASSERT_NE(queueB, nullptr);
  Producer::Ptr producerB = sessionB.createProducer(queueB);
  Consumer::Ptr consumerB = sessionB.createConsumer(queueB);

  EXPECT_NO_THROW(producerB->send(*r1));  // forward the middle element
  TextMessage::Ptr b0 = Message::As<TextMessage>(consumerB->recv());
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ("A-second", b0->text());

  // The application considers r1 handled once forwarded.
  EXPECT_NO_THROW(consumerA->acknowledgeOn(*r1));

  // A's own chain must still hold exactly r0 and r2, in order.
  EXPECT_NO_THROW(sessionA.recover());
  TextMessage::Ptr again0 = Message::As<TextMessage>(consumerA->recv(50000));
  ASSERT_NE(again0, nullptr) << "A lost a message from its own in-flight chain";
  EXPECT_EQ("A-first", again0->text());
  TextMessage::Ptr again1 = Message::As<TextMessage>(consumerA->recv(50000));
  ASSERT_NE(again1, nullptr) << "A lost a message from its own in-flight chain";
  EXPECT_EQ("A-third", again1->text());
  TextMessage::Ptr noneFromA = Message::As<TextMessage>(consumerA->recv(50000));
  EXPECT_EQ(noneFromA, nullptr) << "ZOMBIE: acknowledged message redelivered by recover()";

  // B's own chain is unaffected: exactly one in-flight message.
  EXPECT_NO_THROW(sessionB.recover());
  TextMessage::Ptr bAgain = Message::As<TextMessage>(consumerB->recv(50000));
  ASSERT_NE(bAgain, nullptr);
  EXPECT_EQ("A-second", bAgain->text());
  TextMessage::Ptr noneFromB = Message::As<TextMessage>(consumerB->recv(50000));
  EXPECT_EQ(noneFromB, nullptr);

  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again0));
  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again1));
  EXPECT_NO_THROW(consumerB->acknowledgeOn(*bAgain));
}

// ZZ4: same forward-from-the-middle shape as the previous test, but drives
// it to the point where the (pre-fix) shared downstream node loses its last
// owner while a raw back-pointer (_inFlightTail) still references it —
// review round 2 caught this as a heap-use-after-free under ASan, at the
// write `_inFlightTail->_inFlightNext = message;` in Consumer::recv(). In a
// non-ASan build the same corruption doesn't crash — it just silently drops
// or misroutes messages — so this test also checks functional correctness
// end to end, and is meant to be run under cmake-build-asan as well as
// plain debug/release.
TEST_F(RecoverTest, ForwardedMiddleMessageDestructionDoesNotDangleTail) {
  Connection connection(*_exchange);
  Session &sessionA = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queueA = sessionA.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + "-A");
  ASSERT_NE(queueA, nullptr);
  Producer::Ptr producerA = sessionA.createProducer(queueA);
  Consumer::Ptr consumerA = sessionA.createConsumer(queueA);

  TextMessage a0 = sessionA.createTextMessage("A-first", Message::NOT_PERSISTENT);
  TextMessage a1 = sessionA.createTextMessage("A-second", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producerA->send(a0));
  EXPECT_NO_THROW(producerA->send(a1));

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumerA->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(consumerA->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  // r1 is A's chain tail.

  Session &sessionB = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queueB = sessionB.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + "-B");
  ASSERT_NE(queueB, nullptr);
  Producer::Ptr producerB = sessionB.createProducer(queueB);
  Consumer::Ptr consumerB = sessionB.createConsumer(queueB);

  EXPECT_NO_THROW(producerB->send(*r1));  // forward the tail element
  {
    TextMessage::Ptr b0 = Message::As<TextMessage>(consumerB->recv());
    ASSERT_NE(b0, nullptr);
    // Acknowledge and drop b0 in the same scope: with the pre-fix copy ctor,
    // b0's _inFlightNext could still alias A's chain, and acknowledging it
    // as B's (sole) in-flight message would repoint B's own _inFlightHead
    // at that aliased node while leaving B's _inFlightTail pointing at b0 —
    // which is freed the moment this block ends and the last Ptr to it
    // (this local `b0`) goes away.
    EXPECT_NO_THROW(consumerB->acknowledgeOn(*b0));
  }

  // A receives a third message: appends to A's own tail (still r1). If
  // B's dangling tail aliased anything reachable from here, this recv (or
  // ASan under cmake-build-asan) is where it surfaces.
  TextMessage a2 = sessionA.createTextMessage("A-third", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producerA->send(a2));
  TextMessage::Ptr r2 = Message::As<TextMessage>(consumerA->recv());
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ("A-third", r2->text());

  // B receives a second message of its own: appends to B's tail. This is
  // the exact write (`_inFlightTail->_inFlightNext = message`) the review's
  // ASan run caught as a use-after-free when B's tail was left dangling.
  TextMessage bMsg = sessionB.createTextMessage("B-second", Message::NOT_PERSISTENT);
  EXPECT_NO_THROW(producerB->send(bMsg));
  TextMessage::Ptr b1 = Message::As<TextMessage>(consumerB->recv());
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ("B-second", b1->text());

  EXPECT_NO_THROW(sessionA.recover());
  TextMessage::Ptr again0 = Message::As<TextMessage>(consumerA->recv(50000));
  TextMessage::Ptr again1 = Message::As<TextMessage>(consumerA->recv(50000));
  TextMessage::Ptr again2 = Message::As<TextMessage>(consumerA->recv(50000));
  ASSERT_NE(again0, nullptr);
  ASSERT_NE(again1, nullptr);
  ASSERT_NE(again2, nullptr);
  EXPECT_EQ("A-first", again0->text());
  EXPECT_EQ("A-second", again1->text());
  EXPECT_EQ("A-third", again2->text());

  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again0));
  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again1));
  EXPECT_NO_THROW(consumerA->acknowledgeOn(*again2));
  EXPECT_NO_THROW(consumerB->acknowledgeOn(*b1));
}

// --- Spec 23 round 4 (review round 3, §17): B4 — acknowledgeOn() destroyed
// its own `message` argument mid-function whenever the in-flight chain held
// the last owning reference to it, then kept reading and writing through the
// dangling reference (Consumer.cpp, the two chain-unlink assignments and the
// persistence bookkeeping/logging that follows). This is documented,
// supported usage, not exotic: the class comment on Message::InFlightLink
// promises the chain keeps a message alive "even if the caller drops its own
// Message::Ptr without acknowledging" — acknowledging afterwards via a bare
// Message& is exactly the scenario that promise exists for. Fixed by pinning
// `message` alive (a local shared_ptr copy of the chain's owning reference,
// taken before that reference is overwritten) for the rest of
// acknowledgeOn(). Persistent messages exercise the read of
// message._storageTomId/uuid after unlink — the part of the function that
// read freed memory under ASan in the round-3 review (§17). ---

// ZZ6 (round-3 review, reproduced verbatim): head-of-chain case. r0 is the
// sole in-flight message; dropping the caller's Ptr leaves the chain as its
// only owner, and acknowledging it takes the `_inFlightHead = ...` unlink
// branch.
TEST_F(RecoverTest, AcknowledgeOnSurvivesCallerDroppingOwnPtrHeadOfChain) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m0 = session.createTextMessage("solo", Message::PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));

  Message::Ptr r0 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  // The in-flight chain (Consumer::_inFlightHead) is now r0's only owner
  // once the line below runs.
  Message &ref0 = *r0;
  r0.reset();
  EXPECT_NO_THROW(consumer->acknowledgeOn(ref0));

  EXPECT_NO_THROW(session.recover());
  Message::Ptr none = consumer->recv(50000);
  EXPECT_EQ(none, nullptr) << "acknowledged message must not be redelivered";
}

// Same defect, opposite unlink branch: two in-flight messages, drop the
// caller's Ptr to the *tail* (non-null prev), acknowledge through a bare
// reference. Takes the `prevPtr->_inFlightLink.next = ...` branch instead of
// `_inFlightHead = ...`.
TEST_F(RecoverTest, AcknowledgeOnSurvivesCallerDroppingOwnPtrTailOfChain) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m0 = session.createTextMessage("head", Message::PERSISTENT);
  TextMessage m1 = session.createTextMessage("tail", Message::PERSISTENT);
  EXPECT_NO_THROW(producer->send(m0));
  EXPECT_NO_THROW(producer->send(m1));

  Message::Ptr r0 = consumer->recv();
  Message::Ptr r1 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  // Chain: r0 (head) -> r1 (tail, prev == r0). r1 has a second owner (the
  // test's own r1 Ptr) — drop only the caller's reference to r0's *sibling*,
  // r1, while keeping r0's Ptr alive, so the chain remains r1's sole owner.
  Message &ref1 = *r1;
  r1.reset();
  EXPECT_NO_THROW(consumer->acknowledgeOn(ref1));
  EXPECT_NO_THROW(consumer->acknowledgeOn(*r0));

  EXPECT_NO_THROW(session.recover());
  Message::Ptr none = consumer->recv(50000);
  EXPECT_EQ(none, nullptr) << "both acknowledged messages must not be redelivered";
}

// T1: clearInFlight() is the only defense against an unbounded recursive
// shared_ptr teardown when a Consumer with a long never-acknowledged chain
// is destroyed (letting _inFlightHead's shared_ptr destructor unwind the
// chain node-by-node recurses one stack frame per in-flight message — see
// the CAUTION note on Message::_inFlightNext). Without it, this test
// SIGSEGVs on a stack overflow while the rest of the suite stays green
// (nothing else builds a chain this deep). Non-persistent, to isolate the
// in-flight chain teardown from storage I/O cost.
TEST_F(RecoverTest, DestroyingConsumerWithDeepInFlightChainDoesNotOverflowStack) {
  constexpr int kDepth = 200000;
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  for (int i = 0; i < kDepth; ++i) {
    TextMessage m = session.createTextMessage("m", Message::NOT_PERSISTENT);
    producer->send(m);
  }
  for (int i = 0; i < kDepth; ++i) {
    Message::Ptr r = consumer->recv();
    ASSERT_NE(r, nullptr);
    // None acknowledged — all kDepth messages stay linked in _inFlightNext.
  }

  const auto id = consumer->id();
  consumer.reset();          // drop the test's own reference...
  session.deleteConsumer(id);  // ...and the Session's, so ~Consumer() runs now.
  // Reaching here at all is the assertion: a recursive chain teardown would
  // have crashed the process before this line.
  SUCCEED();
}
