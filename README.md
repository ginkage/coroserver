## What ##

I was wondering how to implement _the_ simplest web server possible (as in, no query parsing, dumb responder), while using at least some of the best practices, like async networking and C++20 coroutines.

I think I've mostly nailed it (sans proper error handling).

## Compiling: ##

```g++ server.cpp -fcoroutines -O3 -o server```

Simple as that.
