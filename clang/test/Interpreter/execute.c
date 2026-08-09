// REQUIRES: host-supports-jit

// RUN: cat %s | clang-repl -Xcc -xc -Xcc -Xclang -Xcc -verify | FileCheck %s
// RUN: cat %s | clang-repl -Xcc -xc -Xcc -O2 -Xcc -Xclang -Xcc -verify| FileCheck %s
int printf(const char *, ...);
int i = 42; err // expected-error{{use of undeclared identifier}}
int i = 42;
struct S { float f; struct S *m;} s = {1.0, 0};
// FIXME: Making foo inline fails to emit the function.
int foo() { return 42; }
int first_label() { goto done; done: return 1; }
int second_label() { goto done; done: return 2; }
void parameter_scope(int Token);
typedef struct Token { int value; } Token;
int parameter_scope_test() { Token t = {3}; return t.value; }
void run() {                                                    \
  printf("i = %d\n", i);                                        \
  printf("S[f=%f, m=0x%llx]\n", s.f, (unsigned long long)s.m);  \
  printf("labels = %d, %d\n", first_label(), second_label());    \
  printf("parameter scope = %d\n", parameter_scope_test());      \
  int r3 = foo();                                               \
}
run();
// CHECK: i = 42
// CHECK-NEXT: S[f=1.000000, m=0x0]
// CHECK-NEXT: labels = 1, 2
// CHECK-NEXT: parameter scope = 3

%quit
