AC_DEFUN([AC_REQUIRE_SOCKET],
[
    AC_SEARCH_LIBS(socket, [xnet bsd socket inet], [have_socket=yes])
    if test x$have_socket = xyes; then
      AC_SEARCH_LIBS(gethostbyname, [bsd socket inet netinet])
      AC_CHECK_MEMBERS([struct linger.l_linger], , , [
#include <sys/types.h>
#include <sys/socket.h>
		       ])
    else
	AC_ERROR([Couldnt find socket functions])
    fi
])