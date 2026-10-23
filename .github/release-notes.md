Форк SA:COOP для GTA San Andreas 1.0 US: сюжет по сети на 2-16 игроков. Основа - оригинальная 0.2.7.

## Что нового

- до 16 игроков, число мест выбирает хост (F9 или `max_players` в `coop\server.ini`, по умолчанию 4)
- машины и прохожие чужой игры появляются только рядом, дальние игроки обновляются реже
- подключение только по IP, при ошибке подключения пишется конкретная причина
- маркер миссии ждёт только тех, кто ближе 200 м, `/go` начинает без остальных
- до трёх напарников рядом с CJ в катсценах
- цвета игроков, команды `/tp`, `/tphere`, `/go`, `/mute`, `/unmute`

### 0.3.1

- в меню есть все разрешения экрана, в том числе 1920x1080; в окне можно играть без рамки на весь экран (`borderless = 1`)
- дальность прорисовки вдвое больше обычной, меняется в `coop\coop.ini` (`draw_distance`, `lod_distance`) и командой `/draw`
- вторая и следующие катсцены миссии снова идут у напарника; катсцена дожидается напарника, который в воздухе
  или в самолёте, а не пропускается. Персонажи напарников стоят только в тех катсценах, где они были в 0.3.0:
  в катсцене, которая идёт сразу за предыдущей, их нет
- продавцы, кассиры, парикмахеры и жильцы в интерьерах у каждого свои: с ними снова можно иметь дело, и копия
  чужого продавца больше не стоит в своём
- камера пассажира, который целится из машины, больше не дёргается: машина напарника плавно поворачивает,
  а прицел пассажира виден другим без рывков
- версия протокола 103: обновиться нужно всем

С оригинальными 0.2.x не совместимо, обновиться нужно всем. Игра втроём и больше пока почти не тестировалась,
если что-то не так, пришлите `coop\coop.log`.

## Установка

Распакуйте `SA-COOP.zip` в папку игры, чтобы `coop.asi`, `install.bat` и папка `coop` оказались рядом с `gta_sa.exe`, и запустите `install.bat`. Подробности в `Инструкция.txt`.

## In English

A fork of SA:COOP for GTA San Andreas 1.0 US: the story over the network for 2-16 players, based on the original 0.2.7. Up to 16 places, other games' cars and pedestrians only near you, connecting by IP with a specific reason when it fails, mission markers that wait only for nearby players (`/go` for the host), up to three partners in cutscenes, player colours, `/tp`, `/tphere`, `/go`, `/mute`. Not compatible with the original 0.2.x. Play with three or more people has barely been tested yet. 0.3.1: every screen resolution in the menu (1920x1080 too) and borderless windowed mode, a longer draw distance (`/draw`), the later cutscenes of a mission play for the partner again, interior shopkeepers and residents are each player's own so they can be dealt with, and no camera shake for a passenger aiming out of a car. Protocol 103: everyone has to update.
