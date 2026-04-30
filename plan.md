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

sandbox 设计：
- 先不急着写规则匹配，第一步只把 execute 的分流设计好
- 识别整条输入是否以 sandbox 开头
- 第二个参数是规则文件路径
- 剩余参数继续按普通命令解析成 job
- execute 增加参数：
    - sandbox_enabled
    - rule_path
- execute 分成两条路径：
    - 普通路径：沿用现在的 fork / exec / waitpid
    - sandbox 路径：沿用同样的 fork / 管道 / builtin 执行，但等待部分单独拆出去
- sandbox 路径中的子进程先不做复杂事，只需要：
    - PTRACE_TRACEME
    - 先停住
    - 再执行 builtin 或 external command
- sandbox 路径中的父进程先单独写一个 while wait 的骨架函数：
    - 输入是当前 job 的直接子进程 pid 列表
    - 循环 waitpid(-1, &status, __WALL)
    - 如果进程退出/被杀，就把它从跟踪集合里删掉
    - 如果是 stop，就先只区分：
        - 初始 stop
        - syscall stop
        - fork/clone/vfork event
- 等 execute 的 sandbox 流程完全通了，再补：
    - 规则文件解析
    - syscall 号和参数读取
    - 命中 deny 后打印 Blocked Syscall

- 具体如何处理 deny
    - 规则文件解析后得到 deny_list
    - deny_list 中有多个 rule
    - 每个 rule 包含：
        - syscall_no
        - 每个参数是否指定
        - 每个参数的类型
        - 参数值
    - 在 wait_for_sandbox_children 的 while 循环里：
        - 只在 syscall entry 时读取寄存器
        - 拿到 syscall 号和参数
        - 逐条匹配 deny_list
        - 如果 syscall 号相同，再继续匹配被指定的参数
        - 全部匹配上就阻止执行并打印 Blocked Syscall

现在还没补 fork/clone/vfork 事件传播