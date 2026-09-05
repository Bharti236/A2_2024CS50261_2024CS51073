# Assignment 2 — Socket Exchange

## Language

C++17, POSIX TCP sockets (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `close`, `shutdown`).

Build with the system C++ compiler (`c++` / `clang++` / `g++`) and pthreads. Tested with a POSIX toolchain (Linux/WSL; intended to run on the course FreeBSD VM).

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
```

`make clean` removes `build/`.

## Start the Exchange Server

```text
./server/run-server 127.0.0.1 5000
```

Arguments: listen address, listen port.

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

## Configuration

No extra files or environment variables. Instruments are only `JNST` and `IMCT`. Quantity and price must be integers in `1 .. 2147483647`. If a launcher is not marked executable, run `chmod +x server/run-server client/run-trader client/run-market-data`.
