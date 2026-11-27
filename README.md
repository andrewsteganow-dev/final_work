В итоговой работе реализованы основные функциональные требования по заданию включая дополнительные и проверки.

Среда выполнения OS Linux OpenSuse 15.4

компиляция выполнена gcc 7.5

gcc++ -std=C++17 -g -Wall minidocker.cpp -o minidocker -pthread -lutil


 Rootless OverlayFS полностью заработал на ядре 5.14
На openSUSE / SLES ядре 5.14.21-150400 в unprivileged user namespace монтирование devpts с newinstance без uid=0,gid=0 или bind-mount поверх overlay требует предварительного монтирования tmpfs на /dev, а для /dev/pts — bind-mount /dev/pts (или devpts с ptmxmode=0666 поверх tmpfs).
Кроме того, внутри overlay каталог /tmp/alpine/dev уже существовал в нижнем слое (lowerdir), поэтому при обращении к merged/dev OverlayFS делал copy_up каталога dev, а mount(..., MS_BIND)
монтируем tmpfs в merged/dev перед созданием узлов
Заменим строки 218–235 в minidocker.cpp так, чтобы:
В merged/dev монтировался чистый tmpfs (mount("tmpfs", dev_dir.c_str(), "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755")).
Внутри этого tmpfs создавались /dev/null, /dev/zero, /dev/random, /dev/urandom, /dev/tty через MS_BIND, а /dev/pts монтировался через devpts (с откатом на MS_BIND /dev/pts, если newinstance запрещён политикой ядра SUSE).
 
 
1. Фоновый (detached) запуск контейнера-генератора логов ./minidocker run -d worker /tmp/alpine /bin/sh -c 'i=1; while true; do echo "worker tick $i"; i=$((i+1)); sleep 1; done' # 2. Проверяем список (должны быть UP: box1 и worker) и ждём 3 секунды ./minidocker ps sleep 3 # 3. Просмотр накопленных логов worker ./minidocker logs worker # 4. Повторное подключение к терминалу box1 (attach с PTY) ./minidocker attach box1 # -> Внутри наберите `ls /root` (увидите overlay_test.txt) и выйдите по Ctrl + ] # 5. Остановка контейнеров по PID (runtime kill) PID_WORKER=$(./minidocker list | awk '$1=="worker" {print $2}') ./minidocker kill $PID_WORKER ./minidocker kill $PID_BOX1 # 6. Финальная проверка списка (статус EXITED) ./minidocker list
 
1. Список запущенных контейнеров (list / ps) — box1 должен быть в статусе UP ./minidocker list # 2. Сравнение Namespaces на хосте (подставьте PID контейнера box1 из вывода list) PID_BOX1=$(./minidocker list | awk '$1=="box1" {print $2}') echo "Host PID of box1: $PID_BOX1" ls -la /proc/$PID_BOX1/ns/ ls -la /proc/self/ns/ # Дескрипторы [...] для pid, mnt, user, uts, ipc, net должны отличаться! ps -o user,pid,cmd -p $PID_BOX1 # Снаружи процесс принадлежит пользователю andrew (UserNS работает) # 3. Проверка OverlayFS (нижний слой чистый, изменения только в upper) ls -la /tmp/alpine/root/overlay_test.txt 2>&1 # Ожидается: No such file or directory cat ~/.minidocker/containers/box1/upper/root/overlay_test.txt # Ожидается: secret-from-box1 # 4. Участие в сети другого контейнера (--net=box1) # Запускаем box2 в сети box1 и отправляем данные на 127.0.0.1:8080 (где слушает nc из box1) ./minidocker run --net=box1 box2 /tmp/alpine /bin/sh -c 'ip a && echo "HELLO_FROM_BOX2" | nc -w 1 127.0.0.1 8080' # Проверяем логи box1 — туда должно прилететь "HELLO_FROM_BOX2"! ./minidocker logs box1



1. Изоляция и Rootless-запуск (clone, user_map, setgroups)
Почему используется clone(), а не fork() + unshare()?
Системный вызов clone() с флагами CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET создаёт дочерний процесс сразу в новых пространствах имён. Для CLONE_NEWPID это критично: сам вызывающий процесс не меняет свой PID namespace, в него попадают только его потомки, поэтому главный процесс контейнера сразу получает PID 1.
Как работает rootless-режим (UID 0 внутри без sudo снаружи)?
Сначала создаётся процесс с CLONE_NEWUSER. Внутри он получает полные привилегии (CAP_SYS_ADMIN и др.) в пределах своих namespaces, что разрешает ему вызывать mount() и chroot().
Снаружи демон записывает маппинг 0 <host_uid> 1 в /proc/<pid>/uid_map и /proc/<pid>/gid_map (предварительно записав "deny" в /proc/<pid>/setgroups — обязательное требование безопасности ядра Linux для непривилегированных пользователей).
2. Файловая система (OverlayFS, chroot, /proc, /dev)
Зачем нужен MS_REC | MS_PRIVATE перед монтированием?
В современных дистрибутивах Linux корень / по умолчанию имеет тип MS_SHARED. Без перевода в MS_PRIVATE все монтирования внутри контейнера распространялись бы на хост.
3.Как устроен слой хранения контейнера?
Используется OverlayFS: базовый образ (rootfs) подключается как lowerdir (только для чтения), а все изменения внутри контейнера пишутся в изолированный upperdir (с опцией userxattr для поддержки rootless-режима).
Почему примонтирован конкретный файл /dev/pts/N, а не devpts newinstance?
Master-дескриптор PTY открывает демон на хосте в корневом пространстве devpts. Если смонтировать внутри контейнера devpts с флагом newinstance, там создастся пустой экземпляр ФС без номера этого терминала, и вызовы ttyname() / tty будут возвращать not a tty. Точечный MS_BIND файла slave PTY сохраняет
Зачем в дочернем процессе нужны setsid() и ioctl(..., TIOCSCTTY, 0)?
Одного dup2() на 0/1/2 недостаточно, чтобы терминал стал управляющим (controlling terminal). setsid() создаёт новую сессию без управляющего терминала, а TIOCSCTTY привязывает к ней slave PTY. Благодаря этому внутри контейнера работают сигналы управления заданиями (Ctrl+C убивает запущенную внутри команду, а не сам /bin/sh).
st_rdev и не раскрывает контейнеру остальные терминалы хоста.
4. Псевдотерминал (PTY), raw-режим и attach
Как устроен проброс терминала в клиент-серверной модели?
Демон владеет master-дескриптором PTY (/dev/ptmx), непрерывно вычитывая из него вывод в файл логов и (если подключён клиент) в UNIX-сокет. Потокобезопасная функция ptsname_r() исключает гонку данных при параллельном запуске нескольких контейнеров.
 
Зачем в дочернем процессе нужны setsid() и ioctl(..., TIOCSCTTY, 0)?
Одного dup2() на 0/1/2 недостаточно, чтобы терминал стал управляющим (controlling terminal). setsid() создаёт новую сессию без управляющего терминала, а TIOCSCTTY привязывает к ней slave PTY. Благодаря этому внутри контейнера работают сигналы управления заданиями (Ctrl+C убивает запущенную внутри команду, а не сам /bin/sh).
 
5. Зачем на клиенте вызывается cfmakeraw()?
Переводит локальный терминал клиента в «сырой» режим (отключает локальное эхо, буферизацию строк и перехват Ctrl+C / Ctrl+Z хостовой оболочкой), чтобы все нажатия клавиш уходили напрямую в PTY контейнера, а комбинация отключения (Ctrl + ], ASCII 0x1d) перехватывалась клиентом.
Сетевая изоляция и --net=<id|pid>
По умолчанию контейнер создаётся с флагом CLONE_NEWNET и поднимает только loopback-интерфейс lo через ioctl(SIOCSIFFLAGS, IFF_UP).  При указании --net=<target> контейнер не создаёт новый сетевой namespace, а входит в user и net namespaces целевого контейнера через системный вызов setns(), позволяя нескольким контейнерам общаться по localhost (аналог Pod в Kubernetes
Таблица дескрипторов идеальна — утечек ресурсов нет.Что сейчас открыто у демона (всего 7 дескрипторов):0, 1, 2 — стандартный ввод/вывод/ошибки самого демона (/dev/pts/0).  3, 4 — слушающий UNIX-сокет демона.  5 (-> /dev/ptmx) и 6 (-> .../box_inter/container.log) — единственный всё ещё работающий в фоне интерактивный контейнер box_inter. Все 10 дескрипторов (ptmx и container.log) и каналы pipe2 от пяти завершившихся контейнеров stress_1...stress_5 были полностью закрыты ядром и демоном без единого зависшего файла или сокета.


ndrew@localhost:~/container> ./minidocker run box1 /tmp/alpine "/bin/sh -i"
STARTED: id=box1 pid=29300
ps aux
^C
andrew@localhost:~/container> ./minidocker run box1 /tmp/alpine "/bin/sh -i"
STARTED: id=box1 pid=29386
Убито
andrew@localhost:~/container> ./minidocker ps
ID    PID    STATUS    COMMAND
box1    29613    UP    /bin/sh -i
andrew@localhost:~/container> ./minidocker attach box1
[ATTACH] Выход: Ctrl + ]
 
/ # ps aux
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/sh -i
    2 root      0:00 ps aux
/ # hostname
box1
/ # ip addr
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
/ # ipcs
 
------ Message Queues --------
key        msqid      owner      perms      used-bytes   messages    
 
------ Shared Memory Segments --------
key        shmid      owner      perms      bytes      nattch     status      
 
------ Semaphore Arrays --------
key        semid      owner      perms      nsems     
 
/ # id
uid=0(root) gid=0 groups=0
/ # ls -la /dev
total 0
drwxr-xr-x    2 root     0               40 Nov 24 16:12 .
drwxr-xr-x    1 nobody   65534           54 Nov 24 15:57 ..
/ #

./minidocker run box1 /tmp/alpine /bin/sh
[ATTACH] Подключено к box1. Выход: Ctrl + ]
/ # ls -la /dev/null /dev/pts
crw-rw-rw-    1 nobody   65534       1,   3 Nov 26 07:41 /dev/null
 
/dev/pts:
total 0
drwxr-xr-x    2 root     0                0 Nov 26 15:42 .
drwxr-xr-x    3 root     0              160 Nov 26 15:42 ..
crw-rw-rw-    1 root     0           5,   2 Nov 26 15:42 ptmx
/ # echo "ok" > /dev/null
/ #
