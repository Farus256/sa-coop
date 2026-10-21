Форк мода SA:COOP для GTA San Andreas 1.0 US: сюжет по сети для 2–16 игроков.

## Что нового (0.3.0, форк)

- **До 16 игроков.** Хост выбирает число мест (2–16, по умолчанию 4) в окне F9 или строкой `max_players` в `coop\server.ini`.
- **Трафик по расстоянию.** Игроки рядом получают состояние друг друга 30 раз в секунду, дальние — дважды в секунду. Машины и прохожие чужой игры появляются только рядом с игроком.
- **Код приглашения — пропуск.** 16 символов `XXXX-XXXX-XXXX-XXXX`, внутри ключ сервера. Без кода или пароля не войти; подбор кода блокируется.
- **Миссии.** Маркер ждёт только тех, кто в 200 м от него; `/go` у хоста начинает без отставших.
- **Катсцены.** До трёх напарников рядом с CJ, у каждого своё место.
- **Цвета игроков** в нике над головой, чате, списке и на радаре.
- **Команды:** `/tp <ник>`, `/tphere <ник>`, `/tphere all`, `/go`, `/mute`, `/unmute`.
- **Микрофон** открывается только по клавише B и закрывается через 30 с тишины.
- **Население и поезда** в группе игроков создаёт одна игра.
- **Всё из оригинальной 0.2.7** включено: предметы в руках, телефон у уха, речь прохожих, полицейские заграждения, сюжетные персонажи как модель игрока, быстрое переподключение и исправления вылетов. Новые функции 0.2.7 работают для всех игроков, а не только для пары.

## Важно

- Версия 0.3.0 не подключается к оригинальным 0.2.x (в том числе 0.2.7): обновиться нужно всем.
- Сервер и протокол проверены автотестами на 16 ботах. Игра с тремя и более игроками на практике ещё не проверялась — присылайте `coop\coop.log`, если что-то не так.

## Установка

Распакуйте `SA-COOP.zip` в папку игры, чтобы `coop.asi`, `install.bat` и папка `coop` оказались рядом с `gta_sa.exe`, и запустите `install.bat`. Подробная инструкция — в `Инструкция.txt`.

## In English

A fork of SA:COOP for GTA San Andreas 1.0 US: the story over the network for 2–16 players. Up to 16 places, distance-based traffic, invite codes that work as tickets, missions that wait only for nearby players (`/go` for the host), up to three partners in cutscenes, player colours, new commands (`/tp <nick>`, `/tphere <nick|all>`, `/go`, `/mute`), a microphone that opens only while B is held. Everything from the original 0.2.7 is included (objects in hand, the phone, pedestrian speech, police roadblocks, story characters as the player model, quick reconnect, crash fixes). Version 0.3.0 does not connect to the original 0.2.x. The server and protocol pass automated tests with 16 bots; play with three or more players has not been tried in practice yet.
