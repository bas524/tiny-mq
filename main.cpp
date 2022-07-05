#include <iostream>
#include <thread>
#include <utility>
#include <Poco/Timestamp.h>
#include <Poco/Format.h>
#include <Poco/Thread.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Util/HelpFormatter.h>
#include "Exchange.h"
#include "Message.h"
#include "Producer.h"
#include "Consumer.h"
#include "Destination.h"
#include "TextMessage.h"

#ifdef ENABLE_TESTS
#include <gtest/gtest.h>
#endif
namespace tiny_mq {
class App : public Poco::Util::ServerApplication {
 public:
  App() : _helpRequested(false) {}

  ~App() override = default;

 protected:
  void initialize(Poco::Util::Application &self) override {
    loadConfiguration();  // load default configuration files, if present
    ServerApplication::initialize(self);
    auto &appLogger = Poco::Logger::get("tiny_mq");
    setLogger(appLogger);
    logger().information("starting up");
  }

  void uninitialize() override {
    logger().information("shutting down");
    ServerApplication::uninitialize();
  }

  void defineOptions(Poco::Util::OptionSet &options) override {
    ServerApplication::defineOptions(options);

    options.addOption(Poco::Util::Option("help", "h", "display help information on command line arguments")
                          .required(false)
                          .repeatable(false)
                          .callback(Poco::Util::OptionCallback<App>(this, &App::handleHelp)));
  }

  void handleHelp(const std::string &, const std::string &) {
    _helpRequested = true;
    displayHelp();
    stopOptionsProcessing();
  }

  void displayHelp() {
    Poco::Util::HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("A simple message queue server");
    helpFormatter.format(std::cout);
  }

  int main(const ArgVec & /*args*/) override {
    Poco::Timestamp start;
    if (!_helpRequested) {
      auto exchange = std::make_unique<Exchange>("./tiny-mq");

      Destination::Ptr destination = exchange->create(destination::Queue, "test");

      size_t readerCnt = 1;
      size_t messageCnt = 100000;

      struct ReaderTask : Poco::Runnable {
        int64_t id{0};
        Consumer::Ptr consumer;
        size_t counter{0};
        Poco::Timestamp::TimeDiff elapsed{0};
        Destination::Ptr _d;
        std::atomic_int64_t &_thrCnt;
        ReaderTask(Destination::Ptr d, std::atomic_int64_t &thrCnt) : _d(std::move(d)), _thrCnt(thrCnt) {}
        void run() override {
          id = ++_thrCnt;
          consumer = _d->createConsumer();
          Poco::Timestamp timeStamp;
          while (true) {
            TextMessage::Ptr newMessage = Message::As<TextMessage>(consumer->recv());
            if (newMessage == nullptr) {
              break;
            }
            poco_information(instance().logger(), Poco::format("[%?d] reader get %s", id, newMessage->text()));
            consumer->acknowledgeOn(*newMessage);
            ++counter;
            elapsed = timeStamp.elapsed();
          }
        }
        void print() const {
          poco_critical(instance().logger(), Poco::format("[%?d] reader elapsed %?d", id, elapsed));
          poco_critical(
              instance().logger(),
              Poco::format("[%?d] read %f message per second", id, (double)counter / ((double)elapsed / (double)Poco::Timestamp::resolution())));
        }
      };

      std::vector<std::pair<Poco::Thread, std::unique_ptr<ReaderTask>>> tasks{static_cast<size_t>(readerCnt)};
      std::atomic_int64_t thrCnt{0};
      for (auto &thr : tasks) {
        thr.second = std::make_unique<ReaderTask>(destination, thrCnt);
        thr.first.setName("reader");
        thr.first.start(*thr.second);
      }

      while (thrCnt < (int64_t)readerCnt) {
        Poco::Thread::sleep(100);
      }

      poco_information(instance().logger(), Poco::format("consumers count %z", destination->consumersCount()));

      struct WriterTask : Poco::Runnable {
        size_t _readerCnt = 0;
        size_t _messageCnt = 0;
        Producer::Ptr _producer;
        std::atomic_int64_t &_thrCnt;
        Poco::Timestamp::TimeDiff writerElapsed = 0;
        int64_t id{0};
        Poco::UUIDGenerator _uuidGenerator;
        WriterTask(size_t readerCnt, size_t messageCnt, Producer::Ptr producer, std::atomic_int64_t &thrCnt)
            : _readerCnt(readerCnt), _messageCnt(messageCnt), _producer(std::move(producer)), _thrCnt(thrCnt) {}
        void run() override {
          while (_thrCnt < (int64_t)_readerCnt) {
            Poco::Thread::sleep(100);
          }
          Poco::Timestamp readerTimeStamp;
          for (size_t i = 0; i < _messageCnt; ++i) {
            TextMessage message(Poco::format("Hello, World! %z-%d", (size_t)Poco::Thread::currentTid(), (int)i));
            message.uuid = _uuidGenerator.createRandom();
            message.reliability = Message::NOT_PERSISTENT;
            message.setProperty("a", property::String("bbb"));
            _producer->send(message);
          }
          writerElapsed = readerTimeStamp.elapsed();
        }
        void print() const {
          poco_critical(instance().logger(), Poco::format("writer elapsed %?d", writerElapsed));
          poco_critical(
              instance().logger(),
              Poco::format("write %f message per second", (double)_messageCnt / ((double)writerElapsed / (double)Poco::Timestamp::resolution())));
        }
      };

      std::vector<std::pair<Poco::Thread, std::unique_ptr<WriterTask>>> wtasks{static_cast<size_t>(readerCnt)};
      for (auto &thr : wtasks) {
        thr.second = std::make_unique<WriterTask>(readerCnt, messageCnt, destination->createProducer(), thrCnt);
        thr.first.setName("writer");
        thr.first.start(*thr.second);
      }

      poco_information(instance().logger(), Poco::format("producers count %z", destination->producersCount()));

      size_t k = (destination->type() == destination::Queue) ? 1 : readerCnt;
      size_t allCnt = 0;

      while (allCnt * k < messageCnt * readerCnt) {
        Poco::Thread::sleep(100);
        allCnt = 0;
        for (auto &thr : tasks) {
          allCnt += thr.second->counter;
        }
      }

      for (auto &thr : wtasks) {
        thr.first.join();
        thr.second->print();
      }

      for (auto &thr : tasks) {
        thr.second->consumer->stop();
      }
      for (auto &thr : tasks) {
        thr.first.join();
        thr.second->print();
      }

      exchange.reset();

      poco_critical(instance().logger(), Poco::format("elapsed time %f", (double)start.elapsed() / (double)Poco::Timestamp::resolution()));

      // waitForTerminationRequest();
    }
    return Application::EXIT_OK;
  }

 private:
  bool _helpRequested;
};
}  // namespace tiny_mq

int main(int argc, char **argv) {
#ifdef ENABLE_TESTS
  if (argc > 1 && strncmp(argv[1], "--gtest", strlen("--gtest")) == 0) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
  }
  const size_t testModeSz = strlen("--test-mode");
  if (std::strncmp(argv[1], "--test-mode", testModeSz) == 0) {
    for (size_t i = 0; i < testModeSz; ++i) {
      argv[1][i] = ' ';
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
  }
#endif
  try {
    tiny_mq::App app;
    return app.run(argc, argv);
  } catch (Poco::Exception &exc) {
    std::cerr << exc.displayText() << std::endl;
    return Poco::Util::Application::EXIT_SOFTWARE;
  }
}