emptywindow: 
	cc -o emptywindow emptywindow.c xdg-shell-protocol.c xdg-decoration-unstable-v1.c -lwayland-client -lrt
clean: 
	rm emptywindow
