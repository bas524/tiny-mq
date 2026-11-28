# Подключение клиента tiny-mq по STOMP 1.2

Черновик по REQ-73-1 v0.11. Проверки перед приёмкой продолжаются.

## Где подключаться

Клиенту на любом языке нужен TCP-сокет на той же машине, где работает
брокер. Тестовое или встраивающее C++ приложение должно вызвать
`tiny_mq::BrokerServer::startLoopback()`, передать клиенту возвращённый
системой порт и оставить объект сервера запущенным на время сеанса.
Адрес сокета — `127.0.0.1`. Обычный запуск `tiny_mq` этот слушатель
не включает и порт не публикует.

Соединение локальное, без TLS и аутентификации. Оставьте `login` и
`passcode` пустыми или опустите их. В этом режиме сервер игнорирует
их значения даже при наличии заголовков.

## Согласование и завершение сеанса

1. Отправьте `CONNECT` или `STOMP` с `accept-version:1.2` и непустым
   `host`. В примере `host:localhost` выбирает единственный виртуальный
   хост, а TCP-адрес задаётся отдельно.
2. Проверьте `CONNECTED` и `version:1.2`. Чтобы согласовать расширение
   tiny-mq `framing-v1`, предложите его в `x-tinymq-capabilities`.
   Если расширение обязательно для клиента, продублируйте его в
   `x-tinymq-required-capabilities` и проверьте, что оно вернулось
   в `x-tinymq-capabilities`. Оба заголовка предложения можно опустить
   для обычного подключения STOMP 1.2.
3. Для штатного завершения отправьте `DISCONNECT` с `receipt` и дождитесь
   `RECEIPT` с совпадающим `receipt-id`. После отправки подтверждения
   сервер закрывает соединение. Больше кадров после `DISCONNECT`
   не отправляйте. [STOMP 1.2](https://stomp.github.io/stomp-specification-1.2.html#DISCONNECT).

В примере `heart-beat:0,0` выключает heartbeat в обоих направлениях.
Сервер всё равно сообщает свои интервалы в `CONNECTED`.
[Согласование heartbeat в STOMP 1.2](https://stomp.github.io/stomp-specification-1.2.html#Heart-beating).

### Пример на Python 3

Сохраните код в `stomp_client.py` и запустите `python3 stomp_client.py <порт>`,
подставив порт, который вернул работающий `startLoopback()`. Нужна только
стандартная библиотека Python. Тайм-аут каждой блокирующей операции — три
секунды. Пример читает поток TCP до границ кадров и рассчитан на ответы
без тела с простыми заголовками. В запросах после заголовков идут пустая
строка и байт NUL.

```python
import argparse
import socket


def read_frame(stream):
    command = stream.readline().rstrip(b"\r\n")
    if not command:
        raise RuntimeError("Соединение закрыто до получения ответа")

    headers = {}
    for line in iter(stream.readline, b""):
        if line in (b"\n", b"\r\n"):
            break
        name, value = line.rstrip(b"\r\n").split(b":", 1)
        headers.setdefault(name, value)

    if stream.read(1) != b"\x00":
        raise RuntimeError("Ожидался конец кадра без тела")
    if command == b"ERROR":
        code = headers.get(b"message", b"unknown").decode("ascii")
        raise RuntimeError(f"Ошибка брокера: {code}")
    return command, headers


parser = argparse.ArgumentParser()
parser.add_argument("port", type=int, help="порт, возвращённый startLoopback()")
port = parser.parse_args().port
if not 1 <= port <= 65535:
    parser.error("укажите порт от 1 до 65535")

with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
    with sock.makefile("rb") as stream:
        sock.sendall(
            b"CONNECT\n"
            b"accept-version:1.2\n"
            b"host:localhost\n"
            b"heart-beat:0,0\n"
            b"x-tinymq-capabilities:framing-v1\n"
            b"x-tinymq-required-capabilities:framing-v1\n"
            b"\n\x00"
        )
        command, headers = read_frame(stream)
        if command != b"CONNECTED" or headers.get(b"version") != b"1.2":
            raise RuntimeError("Ожидался ответ CONNECTED с версией 1.2")
        if headers.get(b"x-tinymq-capabilities") != b"framing-v1":
            raise RuntimeError("Брокер не подтвердил framing-v1")

        sock.sendall(b"DISCONNECT\nreceipt:bye\n\n\x00")
        command, headers = read_frame(stream)
        if command != b"RECEIPT" or headers.get(b"receipt-id") != b"bye":
            raise RuntimeError("Ожидался ответ RECEIPT с receipt-id:bye")
        if stream.read(1) != b"":
            raise RuntimeError("Ожидалось закрытие TCP-соединения")

print("STOMP 1.2 и framing-v1 согласованы, отключение подтверждено.")
```

## Ошибки и границы интерфейса

Брокер сообщает причину в заголовке `message` кадра `ERROR` и закрывает
соединение. При сбое записи клиент может увидеть только закрытие сокета.

| `message` | Что проверить |
| --- | --- |
| `version` | В `accept-version` есть `1.2`. Сервер указывает `version:1.2` в ответе. |
| `syntax` | Указан непустой `host`, кадр и значения заголовков корректны. |
| `capability` | Каждый обязательный токен предложен и поддерживается. Списки содержат уникальные токены через запятую без пробелов. |
| `limit` | Кадр и заголовки укладываются в лимиты брокера. |
| `handshake-timeout` / `heartbeat-timeout` | Рукопожатие завершено вовремя, а согласованные проверки активности выполняются. |
| `unsupported-command` | Клиент отправляет только команды текущей фазы соединения. |

Этот сетевой интерфейс поддерживает рукопожатие, кадрирование, heartbeat
и завершение соединения. Удалённое подключение, отправка сообщений
(`SEND`), подписка (`SUBSCRIBE`), подтверждение доставки и транзакции
через него пока недоступны.
