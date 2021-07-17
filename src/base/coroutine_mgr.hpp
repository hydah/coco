#pragma once
#include <vector>

class ConnRoutine;

class ConnManager {
public:
  ConnManager() = default;
  virtual ~ConnManager();

  virtual void Push(ConnRoutine *conn);
  virtual void Remove(ConnRoutine *conn);
  virtual void Destroy();

private:
  std::vector<ConnRoutine *> conns;
  std::vector<ConnRoutine *> zombies;
};