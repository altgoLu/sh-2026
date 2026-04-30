```
job structrue：
pipe [
    cmd1,
    cmd2,
    cmd3
]

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

先写一个统一的 builtin 执行函数，父进程和子进程都复用：
- cd/env-use/env-exit 直接调用同样的逻辑
- exit 在父进程里返回 1，在子进程里只退出当前子进程

否则执行这样的流程：
- 创建 n-1 个 pipe
- 创建 n 个子进程
- 每个子进程指定：
    - 如果 i > 0，stdin 接 pipes[i - 1][0]
    - 如果 i < n - 1，stdout 接 pipes[i][1]
    - 如果 i = n - 1 且 stdout 是默认，则 stdout 接终端
    - 关闭所有 pipe 的 fd
    - 先判断当前 cmd 是否是 builtin
        - 如果是 builtin，就直接在子进程里执行，然后退出
        - 如果不是 builtin，再 exec
- 主进程 fork 完，关闭所有 pipe（这一步和上一步是并行的）

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

env-use / env-exit 实现流程：
不用管env_path，他的框架代码全都不动。
- env-use
    - 检查 argc == 2
    - 如果 env_active == 0，original_path = strdup(getenv("PATH"))
    - new_path = argv[1] + "/bin:" + original_path
    - setenv("PATH", new_path, 1)
    - free(new_path)
    - env_active = 1
- env-exit
    - 如果未激活则直接返回
    - setenv("PATH", original_path, 1)
    - free(original_path)
    - original_path = NULL
    - env_active = 0
