#ifndef ALLOCA_H
#define ALLOCA_H    
#define alloca(s) __builtin_alloca((s))
#endif