```
job structrue：
pipe [
    cmd1,
    cmd2,
    cmd3
]
background: 1 / 0

cmd structure:
{
    argc,
    argv,
    infile,
    outfile,
    append: 1 / 0
}
```

解析指令：
cat a.txt | grep abc

执行流程：

初始在 execute(&j) 语句。

execute(&j) 返回 int：
- 0 表示继续 shell
- 1 表示退出 shell

先判断是否只有一个 cmd，如果是，再判断是否是：
- cd/env-use/env-exit 在主进程修改状态，返回 0（这部分可以先写 TODO）
- exit，返回 1

否则执行这样的流程：
- 创建 n-1 个 pipe
- 创建 n 个子进程
- 每个子进程指定：
    - 如果 i > 0，stdin 接 pipes[i - 1][0]
    - 如果 i < n - 1，stdout 接 pipes[i][1]
    - 如果 i = n - 1 且 stdout 是默认，则 stdout 接终端
    - 关闭所有 pipe 的 fd
    - exec（这部分可以先写 TODO）
- 主进程 fork 完，关闭所有 pipe（这一步和上一步是并行的）
- 如果没有 &，那么 wait
- 如果有，不阻塞

cd 已经实现

实现外部命令：
- execvp 执行 argv[0] 的命令，然后把 argv 传作参数
- 根据返回值
    - 如果 errno == ENOENT，则 _exit(1)
    - 如果是其它错误，则 _exit(2)
- 如果父进程 waitpid 到了（经过WIFEXITED）
    - 1，则是 command not found
    - 2, 则是 execution error
- 如果不是正常退出，也是 Execution Error