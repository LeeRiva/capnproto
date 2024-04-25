// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "calculator.capnp.h"
#include <capnp/ez-rpc.h>
#include <kj/debug.h>
#include <math.h>
#include <iostream>
#include <thread>
#include <chrono>

class PowerFunction final: public Calculator::Function::Server {
  // An implementation of the Function interface wrapping pow().  Note that
  // we're implementing this on the client side and will pass a reference to
  // the server.  The server will then be able to make calls back to the client.

public:
  kj::Promise<void> call(CallContext context) {
    auto params = context.getParams().getParams();
    KJ_REQUIRE(params.size() == 2, "Wrong number of parameters.");
    context.getResults().setValue(pow(params[0], params[1]));
    return kj::READY_NOW;
  }
};

// some thread management objects, quick'n'dirty in global scope
kj::Own<const kj::Executor> executor;
Calculator::Client calculator{nullptr};
bool threadWaiting{false};

void ThreadFunction(const char *pAddress)
{
  try
  {
    capnp::EzRpcClient client(pAddress);
    calculator = client.getMain<Calculator>();

    executor = kj::getCurrentThreadExecutor().addRef();

    auto& waitScope = client.getWaitScope();

    std::cout << "Thread waiting!" << std::endl;
    threadWaiting = true;

    kj::NEVER_DONE.wait(waitScope);

    std::cout << "Thread shutting down?!? Should never happen!" << std::endl;
  }
  catch (const kj::Exception &kje)
  {
    std::cout << "[ThreadFunction] kj::Exception: " << kje.getDescription().cStr() << std::endl;
  }
}

int main(int argc, const char* argv[]) {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " HOST:PORT [sync]\n"
        "Connects to the Calculator server at the given address and "
        "does some RPCs. Creates the client in a worker thread and uses "
        "kj::Executor do perform calls from main, either using executeSync "
        "or executeAsync (use the 'sync' param to specify the first method)" << std::endl;
    return 1;
  }

  using namespace std::chrono_literals;

  std::thread t(ThreadFunction, argv[1]);

  while (!threadWaiting)
    std::this_thread::sleep_for(100ms);

  std::string line;
  std::cout << "Thread ready, press ENTER to continue" << std::endl;
  std::getline(std::cin, line);

  try
  {
    auto call = []()
    {
      // Make a request that just evaluates the literal value 123.
      //
      // What's interesting here is that evaluate() returns a "Value", which is
      // another interface and therefore points back to an object living on the
      // server.  We then have to call read() on that object to read it.
      // However, even though we are making two RPC's, this block executes in
      // *one* network round trip because of promise pipelining:  we do not wait
      // for the first call to complete before we send the second call to the
      // server.

      std::cout << "Evaluating a literal... ";
      std::cout.flush();

      // Set up the request.
      auto request = calculator.evaluateRequest();
      request.getExpression().setLiteral(123);

      // Send it, which returns a promise for the result (without blocking).
      auto evalPromise = request.send();

      // Using the promise, create a pipelined request to call read() on the
      // returned object, and then send that.
      auto readPromise = evalPromise.getValue().readRequest().send();

      // convert RemotePromise into Promise, see https://github.com/capnproto/capnproto/discussions/1981
      kj::Promise<capnp::Response<Calculator::Value::ReadResults>> kjPromise = kj::mv(readPromise);
      return kjPromise;
    };

    if (!_strcmpi(argv[2], "sync"))
    {
      std::cout << "executeSync..." << std::endl;

      // note: no event loop needed (see executeSync documentation)
      // HOWEVER I get an abort() call if it's not defined..
#if 0
      kj::EventLoop loop;
      kj::WaitScope waitScope(loop);
#endif

      // Now that we've sent all the requests, wait for the response.  Until this
      // point, we haven't waited at all!
      auto response = executor->executeSync(call);
  
      KJ_ASSERT(response.getValue() == 123);
    }
    else
    {
      std::cout << "executeAsync..." << std::endl;

      kj::EventLoop loop;
      kj::WaitScope waitScope(loop);

      for (int i = 0; i < 10; ++i)  // execute that call multiple times
      {
        // Now that we've sent all the requests, wait for the response.  Until this
        // point, we haven't waited at all!
        auto response = executor->executeAsync(call).wait(waitScope);

        KJ_ASSERT(response.getValue() == 123);

        std::cout << "Iteration " << i << std::endl;
      }
    }

    std::cout << "PASS" << std::endl;

    if (1)  // reset capnp objects
    {
      kj::EventLoop loop;
      kj::WaitScope waitScope(loop);

      calculator = nullptr;   // it seems this requires an event loop to be present!
    }
  }
  catch (const kj::Exception &kje)
  {
    std::cout << "[main] kj::Exception: " << kje.getDescription().cStr() << std::endl;
  }

  std::cout << "Done, press ENTER to quit main()" << std::endl;
  std::getline(std::cin, line);

  return 0;
}
