make:
	gcc lsp.c -o lsp
install:
	make
	cp lsp /usr/bin/
