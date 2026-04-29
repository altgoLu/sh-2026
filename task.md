# 实现一个简单的 Shell

截止时间（Deadline）：2026年5月15日23：59

## 提交内容：

1. 代码源文件sh.c：注意代码中应有必要的注释；

2. 实验报告_学号.pdf：简要描述实验中遇到的关键问题及解决方案、印象最深的bugs、或者额外实现的功能等即可（1～2页）。

提交方式：请参考实验页面的具体说明

#提交代码\$make submit  
#提交报告\$make report  
#获取得分\$make score  

\*提交冷却时间2小时

Shell是计算机中运行的一个程序，其提供了用户与操作系统内核交互的接口，通过解释用户输入（prompts）来执行相关操作，如管理文件、运行程序和控制进程等。在本实验中，我们将实现一个简单的命令行Shell，以此来熟悉操作系统的进程管理、以及基本的系统调用。

运行如下命令下载本实验的框架代码（不需要注册NJUGitLab账号）：

\$ git clone https://git.nju.edu.cn/oslab/sh-2026.git  
\$ cd sh-2026

关于实验的疑问，请在GitLab上相关仓库提Issue。

## 实验内容

在本次实验中，我们需要实现一个简化版Shell，其在运行后将不断接受用户输入并进行处理，直到退出为止，即类似如下的使用形式：

## 1. 内置命令（builtin commands）

内置命令是指由Shell本身实现的一些命令或功能（而不是由Shell去调用其它外部程序）。Shell需支持以下两个简单的内置命令：

- `exit`：退出Shell。
- `cd [path]`：接收一个参数，用于切换当前工作目录。若接收的输入为`cd ~`，则切换到主目录。

此外，Shell还需支持基于环境变量的轻量级虚拟环境（virtual environments），以便在隔离的环境中运行外部命令（类似conda activate）。该功能主要通过修改PATH环境变量实现，具体包括以下两个内置命令：

- `env-use [path]`：激活虚拟环境，其中`[path]`为虚拟环境所在目录的绝对路径（可假设`[path]`目录已存在）。该命令执行后，Shell的命令提示符应修改为`(env) sh>`，同时应将`[path]/bin`添加到环境变量PATH的最前面（确保随后执行外部命令时，Shell会优先匹配并执行虚拟环境中的程序）。
- `env-exit`：退出虚拟环境。该命令执行后，Shell的命令提示符应恢复为`sh>`，同时环境变量PATH应恢复为激活之前的状态。

Shell在启动后的环境变量应该仅包括以下几项（可以使用外部命令env打印环境变量信息）：

```
sh>env
PATH=/bin                #查找可执行文件的目录列表（多个目录以：分隔）
HOME=[current directory] #主目录
PWD=[current directory]  #当前工作目录
OLDPWD=[current directory]#上一次工作目录
LANG=en_US.UTF-8         #语言和字符编码设置
SH_VERSION=[some value]  #Shell的版本号
```

其中，PATH的默认值应为`/bin`（注意这里`/bin`后面没有`/`）；目录HOME、PWD和OLDPWD默认值应为当前所处目录的绝对路径（例如，`/home/user/os-lab1`）；SH_VERSION可自行决定版本号格式。

基于上述初始环境变量，虚拟环境的使用示例如下：

```
sh> env-use /home/my_env   # 激活虚拟环境（假设 /home/my_env 已存在）
(env) sh> env
PATH=/home/my_env/bin:/bin
HOME=[current directory]
PWD=[current directory]
OLDPWD=[current directory]
LANG=en_US.UTF-8
SH_VERSION=[some value]
(env) sh> ls               # 此时应优先执行 /home/my_env/bin 中的 ls
(env) sh> env-exit         # 退出虚拟环境
sh>
```

内置命令如果命令格式不正确（例如，`cd` 或 `env-use` 后面没有参数），打印 `Invalid Syntax` 错误信息。

## 2. 执行外部命令

Shell 需要能执行外部的可执行文件，包括执行系统中的某个 GNU Core Utility（例如 ls，cat，wc，sort 等，其路径位于 /bin）、或者某个自己编写的程序（例如 ./a.out 等）。

如果找不到可执行文件（命令不存在），打印 `Command Not Found` 错误信息；如果外部命令执行出错（即返回值不是 0），打印 `Execution Error` 错误信息。

## 3. 重定向 (redirection)

Shell 可通过 `>` 来将某个命令的标准输出 (stdout) 重定向到某个文件中。例如，对于 `cmd > file_path`，若 `file_path` 存在，则清空文件内容然后写入；若不存在则新建文件写入。

类似地，如果命令不存在、或执行出错，打印 `Command Not Found` 或 `Execution Error` 错误信息。在本次实验中，我们只需要让 Shell 支持最基础的 `>` 重定向即可，其它包括 `<` 和 `>>` 在内的重定向符可简单视为 `Invalid Syntax` 错误。

## 4. 管道 (pipe)

Shell 可通过管道 `|` 来连接多个命令。例如，`ls | wc -l` 将通过程序 ls 列出当前目录下的文件，并将此作为程序 wc 的输入，用于进一步统计文件的数量。

Shell 应支持任意多个命令通过管道进行连接。如果出现两个连续的 `|` 符号时可认为不符合语法规范（两个 `|` 中间可能有空格，例如 `| |`），打印 `Invalid Syntax` 错误信息。对于管道中的每一个命令，如果其不存在、或执行出错，打印 `Command Not Found` 或 `Execution Error` 错误信息。

## 5. 沙盒执行

为了防止“恶意”程序的危害，Shell 需提供一个 `sandbox` 命令来在受限的系统调用下执行某个可执行文件：

例如，以受限方式执行 `ls -l` 或者 `./a.out | cat`

```
sh> sandbox rule.txt ls -l
sh> sandbox rule.txt ./a.out | cat
```

其中，`cmd` 是 Shell 支持的某个命令，`rule` 是一个纯文本规则文件，用于描述当前系统调用的限制规则。此时，如果 `cmd` 执行过程中（包括其创建的所有子进程）调用了一个被禁止的系统调用，Shell 需立即终止其运行（注意此时被禁止的系统调用不应被执行），并打印 `blocked syscall` 错误信息。

`sandbox` 按一个简单的黑名单模式来工作，即默认允许所有的系统调用执行，只有明确匹配 `deny` 规则的才会被禁止。相应地，规则文件中每一行代表一个具体的 `deny` 规则：

```
deny:[syscall name] [parameter value]
```

其中，`[syscall name]` 是被禁止的 Linux 系统调用名称，`[parameter value]` 进一步指定仅禁止该系统调用的某个参数取值，以形如 `argN=...` 的方式表示（这里用 `argN` 表示第 N 个参数，从 0 开始编号）。若没有提供 `[parameter value]`，则默认禁止所有可能的输入参数取值。例如：

```
# 不提供参数
deny:fork          # 禁止所有 fork 调用
deny:open          # 禁止所有 open 调用
# 提供参数
deny:write arg0=1 arg1="abc"   # 仅禁止向文件描述符为 1 的文件 (stdout by default) 写入 "abc"（同时满足）
deny:open arg0="/etc/passwd"   # 仅禁止 open 特定文件
deny:execute arg0="/.a.out"    # 仅禁止执行特定可执行文件
```

为简化规则设计，规则文件不需要支持通配符、正则表达式等灵活的规则表示；并且可以假设同一个系统调用名称只会在规则文件中出现一次（即按顺序扫描规则文件，找到第一个匹配规则时就可停止）。

在本次实验中，sandbox 仅需要支持对以下系统调用的阻塞即可，这里可以查看具体系统调用的参数信息（如果你使用的是 32 位系统或其他架构的设备，系统调用号会不同，请自行查找相关信息用于本地调试，提交请时使用 x86-64 的系统调用号）：

```
// syscall number (Linux x86-64) and syscall name
0    read
1    write
2    open
22   pipe
32   dup
56   clone
57   fork
59   execve
83   mkdir
90   chmod
```

## 6. 错误信息输出

在 Shell 执行过程中遇到错误时，需要由 Shell 主进程调用框架代码中的如下函数打印错误信息（不要自己编写 `printf()` 或 `perror()` 打印错误信息）：

- 命令格式错误 **Invalid Syntax**：对于接收不合法的内部命令参数的情况（例如，`cd` 或 `ls || cat`），使用 `print_invalid_syntax()` 打印错误信息；
- 命令未找到 **Command Not Found**：对于接收不存在的内部或外部命令的情况（例如，`abc 1`），使用 `print_command_not_found()` 打印错误信息；
- 命令执行失败 **Execution Error**：对于命令执行失败的情况（即进程返回值不等于 0），使用 `print_execution_error()` 打印错误信息；
- 系统调用阻塞 **Blocked Syscall**：对于检测到需阻塞系统调用的情况，使用 `print_blocked_syscall()` 打印错误信息（注意需要将系统调用名称及其当前参数取值传递给该函数）。

## 其它说明

在实现上述 Shell 的过程中，你可能会用到以下一些系统调用。关于这些系统调用或 C 标准库等 API 的说明，可以查看 Linux man pages 或询问 AI：

- 用于进程创建的 `fork()` 和 `execve()`（或 exec 系列函数），也可以使用 POSIX 提供的 `posix_spawn()` 等
- 用于进程结束的 `exit()` 等
- 用于等待进程状态改变的 `wait()` 和 `waitpid()` 等
- 用于观察和控制另一个进程执行的 `ptrace()` 等
- 用于操作文件描述符的 `open()`、`close()` 和 `dup()` 等
- 用于改变当前工作目录的 `chdir()` 等
- 用于修改环境变量的 `setenv()` 和 `putenv()` 等

C 标准库中有一个叫做 `system()` 的 API 可以用于执行一个给定的 Shell 命令；Linux 还提供一个叫做 `seccomp()` 的系统调用来限制进程可使用的系统调用（libseccomp 提供了一个更易用的库）。当然，在本实验中你不能使用这些 APIs。

对 Shell 功能如有不清楚的，可以参考自己系统 Shell 的对应执行行为（但注意本实现的 Shell 和一般 Shell 的规格说明存在不同）。Bash Reference Manual 给出了 bash 这一常见 Shell 的简要功能描述，可供参考。

## 一些需要注意的实现细节

### 输入处理

- 可以使用 `getline()` 来获得用户输入；
- 在 `getline()` 返回 -1 时，代表读取失败，此时请结束程序；

### 输出和错误信息

- 使用框架代码中的函数打印各种错误信息；
- 不需要额外使用 `printf()`，所有打印信息都可以通过提供的 `printf_xxx()` 函数完成，如果一定要使用 `printf()`，请在每次使用 `printf()` 后使用 `fflush(stdout)` 清空输出缓冲区，并在报告中解释为什么需要使用 `printf()`；
- sandbox 功能使用 `print_blocked_syscall()` 打印系统调用信息，将系统调用名称和参数值作为字符串 `char*` 传递给该函数（可以借助 `sprintf()` 或 `snprintf()` 函数），具体规则如下：
  - 如果本身是字符串（`char*` 等），添加引号，如 `"Hello World\""`；
  - 如果是整数或浮点数、文件描述符等数字类型（包括 int, unsigned long, size_t, mode_t 和 off_t 等），直接转换为字符串，如 `"123"`；
  - 如果是指针等地址类型，转换为十六进制字符串（包括 `char**` 和 `int*` 等），前面加 @x，如 `"@x8f"`；
  - 其他类型都转换为十六进制字符串，前面加 @x，如 `"@x8f"`；

### 命令合法性检查

- 在执行每个命令前应该先做合法性检查。例如，判断 `|` 和 `>` 出现的位置是否合适，`echo 1 > | a.txt` 就是一个不合法的命令。如果命令不合法，直接输出 `Invalid Syntax` 错误信息，不要执行命令的任何一部分。
- 注意多个需求点之间的可能“组合”情况，我们不要求你的实现能处理所有可能的 corner case，但应该在一些简单和常见的情况下按预期工作；

### 其它

- 注意内存的分配和回收，不然会出现各种奇奇怪怪的问题；
- gdb 和 valgrind 会让你事半功倍。

## 结果评估

我们会在一批具有不同难度的测试用例上评估你实现 Shell 的功能正确性、以及对一些常见组合场景的支持情况，各测试用例之间不会互相影响。我们强烈建议你在提交代码之前，先根据实验手册编写相关的测试用例在本地进行测试。

以下给出了一些简单的测试，假设当前Shell可执行文件位于`/home/user/lab-1`，该目录下还存在目录`a`，并且目录`a`下还有三个子目录`a1`、`a2`和`a3`。

### 1. 内置命令

```
sh> cd a
sh> ls
a1 a2 a3
sh> cd ~
sh> ls
a
sh>

sh> cd
Invalid Syntax
sh>

sh> env
PATH=/bin
HOME=/home/user/lab-1
PWD=/home/user/lab-1
OLDPWD=/home/user/lab-1
LANG=en_US.UTF-8
SH_VERSION=1.9
sh> env-use /home/user/lab-1
(env) sh>
(env) sh> env
PATH=/home/user/lab-1/bin:/bin
HOME=/home/user/lab-1
PWD=/home/user/lab-1
OLDPWD=/home/user/lab-1
LANG=en_US.UTF-8
SH_VERSION=1.9
(env) sh> env-exit
sh>
```

### 2. 执行外部命令

```
sh> cd a
sh> rm -r a1
sh> ls
a2 a3
sh>
sh> cd a
sh> rm -r b
rm: cannot remove 'b': No such file or directory
Execution Error
sh>
```

外部命令在执行过程中可能会通过错误流（stderr）输出一些信息。Shell不需要处理这些信息，只需要在合适的位置使用框架代码中的`print_`系列函数打印实验手册中要求的错误信息即可。

### 3. 重定向

```
sh> cd a
sh> ls > 1.txt
sh> cat 1.txt
1.txt
a1
a2
a3
sh>

sh> cd a
sh> ech 1 | ls > 1.txt
Command Not Found
sh> cat 1.txt
1.txt
a1
a2
a3
sh>
```

### 4. 管道

```
sh> echo hello | cat
hello
sh>

sh> cd a
sh> ech hello | ls
Command Not Found
a1 a2 a3
sh>

sh> yes | head -n 5
y
y
y
y
y
sh>
```

注意Shell有输出各类错误信息的需求，这会导致Shell的管道执行输出结果和常见Shell不太一致，但管道本身需实现的行为和常见Shell是一致的。

### 5. 沙盒执行

假设当前可执行文件`a.out`的源代码为：

```c
#include<stdio.h>
int main(){
    printf("hello world");
    return 0;
}
```

如果规则文件`rule.txt`为：

```
deny:write
```

则

```
sh> sandbox rule.txt ./a.out
Blocked Syscall: write 1 "hello world" 11
sh>
```

如果规则文件`rule.txt`为：

（空文件）

则

```
sh> sandbox rule.txt ./a.out
hello world
sh>
```