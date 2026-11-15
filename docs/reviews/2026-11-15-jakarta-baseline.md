# Базовый аудит tiny-mq перед Jakarta Messaging 3.1

Класс K2. Редакция кода `9f7983c0054636a6686f9928831c8c53dfbd3679`, ветка main.
Дата сверки 2026-11-15. [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md)
и [карта покрытия](../jms-spec/README.md) опираются на эту базу.

## Что подтверждено

| Область | Состояние | Источник |
|---|---|---|
| Сборка и тесты | C++20, Ninja, зависимости vcpkg. GoogleTest встроен в `tiny_mq`; CTest тестов не находит. 157 тестов из 26 наборов прошли и в Debug, и в RelWithDebInfo после пересборки из временной копии. | [CMakeLists.txt](../../CMakeLists.txt), [main.cpp](../../main.cpp), [CI](../../.github/workflows/ci.yml) |
| Сообщения | Модель заголовков, пять специализированных тел, свойства и собственный компилятор селекторов. Требуется сверка преобразований, всех продукций selector и Java API. | [Message.h](../../Message.h), [MessageProperty.cpp](../../MessageProperty.cpp), [Selector.cpp](../../Selector.cpp), [70](../jms-spec/70-jakarta-message-contract.md) |
| Назначения | Локальные очереди и топики, durable storage и фильтры. ADR-0005 фиксирует один consumer на очередь. Shared, browse и полный temporary lifecycle предстоит реализовать или подтвердить по стандарту. | [Destination.cpp](../../Destination.cpp), [Session.cpp](../../Session.cpp), [ADR-0005](../../arch/0005-session-threading-model.md), [71](../jms-spec/71-jakarta-destinations.md) |
| Доставка | Sync C++ receive, локальные ack/tx, recover, priority, TTL, delay, redelivery и DLQ покрыты разными C++ тестами. Гарантии 3.1 требуют сверки на границах send и отказа. | [tests](../../tests), [OpenSpec redelivery](../../openspec/specs/message-redelivery/spec.md), [72](../jms-spec/72-jakarta-delivery.md) |
| Сеть и Java | STOMP 1.2 и protobuf описаны ADR. Текущая сборка не даёт отдельного сетевого брокера, удалённого клиента, Java provider, XA и EE resource adapter. | [ADR-0001](../../arch/0001-wire-protocol-protobuf.md), [CMakeLists.txt](../../CMakeLists.txt), [73–77](../jms-spec/README.md#карта-покрытия) |
| Принятые факты | Две capability-спецификации, 11 требований и 0 ошибок в новом валидаторе. У прежних 11 требований отсутствуют стабильные ID, выданы предупреждения. | [OpenSpec](../../openspec/README.md) |

## Проверенные расхождения

- [Producer.cpp](../../Producer.cpp#L48) назначает `JMSDeliveryTime` от commit для transactional send,
  а при нулевой задержке использует 0. §3.4.13 и §7.9 задают абсолютное время от send;
  видимость после commit проверяется отдельно. [Норма](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html)
- [Consumer.cpp](../../Consumer.cpp#L366) переиспользует публичный `deliveryTime` для очередной
  попытки backoff. [OpenSpec](../../openspec/specs/message-redelivery/spec.md) описывает потерю
  состояния счётчика и backoff при рестарте. SDD 72 уточняет исходный заголовок и внутренний дедлайн.
- [Connection.cpp](../../Connection.cpp#L31) хранит флаг start/stop, а [Consumer::recv](../../Consumer.cpp#L71)
  не проверяет его. Для Java API нужны барьеры доставки и проверка повторных вызовов.
- [main](../../CMakeLists.txt) в данном macOS окружении зависит от отдельной локальной правки
  [Poco/Expat](../../external/poco/CMakeLists.txt). Чистая копия с установленными зависимостями
  не генерируется из-за отсутствующего `EXPAT::EXPAT`. Debug и RelWithDebInfo собраны с уже существующей
  локальной правкой, скопированной во временную копию.

## Как проверена база

Временная копия создавалась из `git archive HEAD`; в неё скопирован существующий локальный
`external/poco/CMakeLists.txt`. Команда CMake использовала установленный prefix
`cmake-build-debug/vcpkg_installed/arm64-osx` из рабочей копии. После сборки из build-каталога
выполнено `./tiny_mq --gtest_filter='-*Bench*'`. Проверка фиксирует текущее C++ поведение.
TCK, новые критерии Jakarta 3.1 и замеры производительности остаются предметом реализации.

Прямые профильные пробы Codex вернули `AEF_OK` для OpenAI `gpt-6.1-sol`,
`deepseek` (`deepseek-flash`) и `deepseek-pro` (`deepseek-v4-pro`) с отключёнными proxy-переменными.
DeepSeek Pro отдельно проверил охват по публичному описанию. Выводы о времени send, XA и EE
сверены с финальным текстом. Правила noLocal, обязательность JMSXDeliveryCount и политику QueueBrowser
в постановках определяет спецификация, включая случаи, где вывод внешнего анализа расходился с ней.

Тесты кита `tests/run_all.sh` прошли во временной копии на его редакции с установленными
проверяющими модулями. Проектные проверки подтвердили аргументы всех трёх обёрток, отключение
переменных прокси и интерактивный режим. Синтетический critic handoff для SDD 70 записал
`critic.input.json` и сгенерировал команду для OpenAI в пути с пробелом и апострофом.
Новые SDD проходят `trace.py sdd` и `trace.py input`. Синтетический прогон подтверждает
маршрутизацию и не присваивает SDD вердикт критика.
