// RUN: rm -rf %t  &&  mkdir -p %t
// RUN: %target-build-swift -swift-version 4 -enable-experimental-conditional-conformances %s -o %t/a.out
// RUN: %target-run %t/a.out 2>&1 | %FileCheck %s -check-prefix=CHECK_WARNINGS
protocol P {
  func foo()
}

struct X : P {
  func foo() { print("X.P") }
}

struct Y<T> {
  var wrapped: T
}

extension Y: P where T: P {
  func foo() { wrapped.foo() }
}

func tryAsP(_ value: Any) {
  if let p = value as? P {
    p.foo()
  }
}

extension Dictionary: P where Value == (Key) -> Bool {
  func foo() { }
}


let yx = Y(wrapped: X())

// CHECK_WARNINGS: warning: Swift runtime does not yet support dynamically querying conditional conformance ('_T01a1YVMn': '_T01a1P_pD')
tryAsP(yx)
