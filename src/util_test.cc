#include "util.h"

#include<cassert>

int main()
{
  using namespace Sim;

  //
  assert(make_random_filename(10).size() == 10);
  assert(make_random_filename(20).size() == 20);

  //
  bool bleh = false;
  {
    Defer _([&bleh]{
      bleh = true;
    });
    assert(bleh == false);
  }
  assert(bleh == true);

  //
  assert("root" == uid_to_username(0));
  assert(0 == group_to_gid("root"));
  assert(user_is_member("root", 0, "root"));
}
