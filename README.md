<h2><p align="center">lsp</p>  </h2>   
<p align="center">ls -lh clone</p>

Example
```
drwxr-xr-x  user:user  428.3 MB  2h ago   pictures
drwxr-xr-x  user:user  6.8 GB    40m ago  videos
-rw-r--r--  user:user  39.0 KB   2d ago   dog.png
```

(with colored output, depending on size and file type)

Comparasions

- **5 times** faster than `du -h --max-depth 1`.
- Exactly as fast as `ls -lh` (`ls -lh` doesn't calculate recursively!).

Features

- Recursive size. Calculates the size of entire directory contents.
- Relative time format, instead of static dates (example: `2mo ago`).
- Columned output, separated by two spaces, easily readable.
- Simplified usage. Only two main options: `-h` for hidden files, and `-i` for inodes.

<sub>CC BY-SA 4.0</sub>
