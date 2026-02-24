# Comparisons

This is an experimental foray into a well-explored area.
Existing projects take different approaches that are interesting to explore.

## OpenMP

[OpenMP](https://www.openmp.org) uses language extensions (pragmas) to explicitly parallelize critical sections of code.
It feels similar to the CUDA programming model in this way.
I wanted to do something that was less "invasive" / framework-y, and support more general multiprocessed applications, rather than more scientific computing.

## MPI, libfabric

Open MPI applications are launched as some number of instances across some number of resources.
Each instance ("rank") can communicate with other ranks, and the framework handles IPC over the network or local transports.

This requires all ranks to be launched at once, and ranks are typically executed from the same binary.
Rather than different behavior based on binary and arguments to the program, ranks typically diverge based on checking their own rank ID.

For example, a code path may include a check against rank ID 0:

```c
do_my_chunk_of_work(my_rank_id);
barrier();
if (my_rank_id == 0) {
	printf("all chunks of work done\n");
}
```

This ensures the log line is printed only once.

## Multithreaded actor model

Actor model frameworks support a model where heterogeneous components communicate through message passing.
In my experience, general actor model frameworks support a single process with multiple threads.
This carries limitations discussed elsewhere, and I think threadprocs would be a fruitful tool to implement such a framework with.

## Google Serviceweaver

I was excited by the [whitepaper](https://dl.acm.org/doi/10.1145/3593856.3595909) when published, but the [Serviceweaver project](https://serviceweaver.dev) is now defunct.
It seems like the model's biggest drawback was that it was an invasive framework that required application knowledge.
It has some drawbacks, but threadprocs could capture some of its benefits.
