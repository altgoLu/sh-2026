# OS Lab Shell 实验报告

## 实现概述

本实验实现了一个简化版 Shell，核心代码集中在 `sh.c`。整体流程分为输入读取、词法分析、命令解析、执行计划检查和命令执行几部分。输入通过 `getline()` 获取；`tokenizer()` 将输入拆分为普通参数、管道符和重定向符；`parser()` 再将 token 组织成 `job` 和 `command` 结构。一个 `job` 表示一条可能包含管道的命令链，一个 `command` 保存参数列表、输入输出文件和重定向模式。

执行部分统一由 `execute()` 负责。对于单个内置命令，`cd`、`exit`、`env-use`、`env-exit` 会在父进程中执行，以便正确修改当前 Shell 的状态。对于外部命令和管道命令，Shell 会先创建所需数量的 pipe，再 fork 出每个子进程，在子进程中用 `dup2()` 设置标准输入输出，最后通过 `execvp()` 执行外部程序。父进程关闭不再使用的 pipe，并根据子进程退出状态打印 `Command Not Found` 或 `Execution Error`。

重定向部分实现了实验要求中的基础 `>` 输出重定向。`<` 和 `>>` 会被解析出来，但在执行计划检查阶段被判为非法语法，符合本实验中只要求支持基础输出重定向的规格。

环境变量部分在 Shell 启动时通过 `clearenv()` 建立一个受控的初始环境，只保留 `PATH`、`HOME`、`PWD`、`OLDPWD`、`LANG` 和 `SH_VERSION`。`env-use` 会把虚拟环境目录下的 `bin` 添加到 `PATH` 前端，`env-exit` 则恢复激活前的 `PATH`。路径处理统一收敛到了 `expand_user_path()` 和相关辅助函数中，用来处理绝对路径、相对路径、`~` 展开和 `realpath()` 规范化，避免 `cd`、`env-use` 和 sandbox 各自维护不同的路径逻辑。

## Sandbox 设计

`sandbox` 是本实验中最复杂的部分。Shell 在主循环中识别形如 `sandbox rule.txt cmd ...` 的输入，先加载规则文件，再将剩余 token 按普通命令继续解析为 `job`。执行时子进程会先调用 `PTRACE_TRACEME` 并通过 `SIGSTOP` 暂停，父进程在 `wait_for_sandbox_children()` 中接管 tracee，使用 `PTRACE_SYSCALL` 在系统调用入口和出口处暂停。

规则文件被解析为 `deny_list`。每条规则记录系统调用号、系统调用名称以及可选参数约束。参数支持整数和字符串两类。匹配时先比较系统调用号，再逐个比较规则中指定的参数。对于字符串参数，父进程通过 `PTRACE_PEEKDATA` 从 tracee 地址空间读取内容。`write` 的第二个参数比较比较特殊：它不是以 `\0` 结尾的字符串，而是 `(buf, len)` 形式的缓冲区，所以实现中用 `arg2` 指定的长度读取字节并进行 `memcmp()`，打印时也按实际长度格式化。

为了覆盖被监控程序继续创建子进程的情况，父进程在设置 ptrace options 时启用了 `PTRACE_O_TRACEFORK`、`PTRACE_O_TRACEVFORK` 和 `PTRACE_O_TRACECLONE`。当收到 fork/clone/vfork 事件后，通过 `PTRACE_GETEVENTMSG` 获取新 pid，并加入 `tracee_state` 表中继续追踪。这样 sandbox 不只监控最初 fork 出来的进程，也能覆盖其后代进程。

路径型参数是 sandbox 中容易出错的点。`execve`、`open`、`mkdir`、`chmod` 的 `arg0` 表面上是字符串，语义上通常是路径。简单 `strcmp()` 会漏掉相对路径、绝对路径、`/bin/ls` 和 `/usr/bin/ls` 这类等价情况。因此实现中对路径型参数先尝试字面匹配，再基于 tracee 的当前工作目录解析相对路径，并对已经存在的路径使用 `realpath()` 做规范化比较；`execve` 还额外兼容 basename 相同的情况，以适应不同程序传入 `execve` 的路径形式。

## 关键问题与解决方案

第一个关键问题是命令表示。最初如果直接边解析边执行，很难处理管道和重定向组合。因此实现中先构造 `job`，再统一检查执行计划，最后执行。这样管道、重定向和内置命令可以共享同一套执行入口，错误处理也更集中。

第二个关键问题是内置命令的执行位置。`cd`、`env-use`、`env-exit` 必须修改 Shell 自身状态，所以单独出现时需要在父进程执行；但如果内置命令出现在管道中，也需要能在子进程路径里执行并退出。实现中用 `run_builtin(cmd, in_parent)` 统一处理这两类情况，`exit` 在父进程中返回退出标志，在子进程中只结束当前子进程。

第三个关键问题是 sandbox 的 tracee 管理。最初只保存了直接子进程 pid，无法覆盖被执行程序内部的 fork/clone。后来将每个被追踪进程抽象为 `tracee_state`，记录 pid、是否存活、当前是否处于 syscall entry/exit 以及是否设置过 ptrace options。这样父进程可以动态追加新的 tracee，并在命中 deny 规则时杀掉所有仍然存活的 tracee。

第四个关键问题是引号和规则字符串。OJ 中存在 `bash -c 'ls | head -n 1'` 这类输入，如果 tokenizer 不理解引号，会把引号内的管道错误地当作 Shell 自己的管道。实现中为 tokenizer 增加了单引号和双引号处理，使引号内内容作为同一个 WORD token。规则文件中也可能出现 `arg1="hello world"` 这样的带空格字符串，因此规则解析不能简单使用按空白分割的 `strtok()`，而是改为游标式解析。

## 印象最深的 Bugs

最典型的 bug 是 `write` 系统调用参数的处理。最开始把 `write` 的 `arg1` 当作普通 C 字符串读取，结果在输出没有 `\0` 结尾时会读多，导致匹配和打印都不稳定。正确做法是结合 `arg2` 的长度读取固定字节数。这个 bug 暴露出系统调用参数不能只看 C 类型表面含义，还必须理解该系统调用对参数的具体解释。

另一个比较典型的 bug 是 `execve arg0` 的路径匹配。规则文件中的路径和实际 `execve` 参数可能形式不同，但指向同一个文件。只做字符串精确匹配会导致 `./b/hello` 这样的相对路径和绝对路径不匹配，也会导致 `/bin/ls`、`/usr/bin/ls` 这类符号链接或目录布局差异下的等价路径漏判。最终通过“先按 tracee cwd 解析，再对存在路径做 `realpath()`，最后比较规范化结果”的方式解决了这个问题。

还有一个 bug 来自 `bash -c` 测试。由于最初 tokenizer 不支持引号，`bash -c 'ls | head -n 1'` 会被错误解析成 Shell 自己的管道，导致 sandbox 监控对象和预期完全不同。加入引号处理后，这类命令能正确作为 `bash` 的参数传递。

最后两处修正把分数从 97.18% 推到 99.76%。一是 `print_blocked_syscall` 打印指针型参数时的前缀：早期按文档措辞用 `@x`，后来改用 `0x` 才与评测对齐。二是 sandbox 子进程第一次 `execve` 的处理：之前把它当作 Shell 启动命令的"引导"动作主动跳过，但评测期望这次 `execve` 同样要参与规则匹配，于是去掉了跳过逻辑，让子进程从第一次 `execve` 起就被规则约束。

## 测试情况

本地主要使用 `make sh` 编译，并通过构造输入文件喂给 `./sh` 的方式测试。覆盖的用例包括：

- 基础外部命令：`echo`、`ls`、命令不存在和非零退出。
- 内置命令：`cd ~`、`cd -`、`env-use`、`env-exit`。
- 管道和重定向：`ls | sort`、`echo hi > file`、非法管道和非法重定向。
- sandbox：禁止 `write`、禁止 `execve`、带参数的 `deny:write arg0=1 arg1="..."`、路径型 `execve arg0`、以及被监控程序内部继续 fork/exec 的情况。

最后通过 `make submit` 和 `make score` 在 OJ 上验证。当前主要功能已经完成，分数达到 99.76%。
