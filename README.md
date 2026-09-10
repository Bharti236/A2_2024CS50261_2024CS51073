# Assignment 2 — Socket Exchange

## Language

C++17, POSIX TCP sockets (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `close`, `shutdown`).

I/O: the Exchange Server uses **`kqueue()` on FreeBSD** (the course VM) and `poll()` on other POSIX systems such as Linux. Trader and market-data clients still use a receive thread plus stdin so `BOUGHT` / `SOLD` / `TRADE` can arrive while the user is typing.

Build with the system C++ compiler (`c++` / `clang++` / `g++`) and pthreads.

## Build

From the submission directory:

```text
make
```

This produces:

```text
build/exchange-server
build/trader
build/market-data
build/conn-gen
```

`make clean` removes `build/`.

## Start the Exchange Server

```text
./server/run-server 127.0.0.1 5000
```

Arguments: listen address, listen port.

For large idle-connection tests, raise the process file-descriptor limit first, for example:

```text
ulimit -n 131072
```

The server also tries `setrlimit(RLIMIT_NOFILE)` at startup and prints the limit it obtained.

## Start a Trader Client

```text
./client/run-trader 127.0.0.1 5000 alice
```

Arguments: server address, server port, username.

The client connects and sends `LOGIN <username>`. Type protocol commands on stdin (`BUY`, `SELL`, `CANCEL`, `QUIT`). Server replies and asynchronous `BOUGHT` / `SOLD` lines are printed as they arrive.

## Start a Market-Data Client

```text
./client/run-market-data 127.0.0.1 5000 JNST
```

Arguments: server address, server port, instrument (`JNST` or `IMCT`).

The client connects and sends `SUBSCRIBE <instrument>`. Type `SUBSCRIBE`, `UNSUBSCRIBE`, or `QUIT` on stdin. `TRADE` updates print as they arrive.

## Bonus: idle connection generator

Creates and holds TCP connections to the server **without** sending `LOGIN`, `SUBSCRIBE`, or any other application message.

```text
./bonus/run-conn-gen 127.0.0.1 5000 10000
```

Arguments: server address, server port, connection count. Optional fourth argument: comma-separated local IPv4 addresses to bind as sources (to avoid ephemeral-port exhaustion):

```text
./bonus/run-conn-gen 127.0.0.1 5000 70000 127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4
```

If that argument is omitted and the count is large, the generator rotates `127.0.0.1` … `127.0.0.N` by itself. Linux usually treats those as loopback already. On FreeBSD, extra aliases are typically required:

```text
ifconfig lo0 alias 127.0.0.2
ifconfig lo0 alias 127.0.0.3
```

The program prints `established <n>` on stdout when the ramp finishes, then keeps the sockets idle until it is killed (Ctrl-C / SIGINT).

Raise `ulimit -n` on **both** the server and the generator. The listen backlog is 8192; if many connects fail during a burst, also raise `kern.ipc.somaxconn` on FreeBSD.

## Configuration

No extra files or environment variables. Instruments are only `JNST` and `IMCT`. Quantity and price must be integers in `1 .. 2147483647`. If a launcher is not marked executable, run `chmod +x server/run-server client/run-trader client/run-market-data bonus/run-conn-gen`.
