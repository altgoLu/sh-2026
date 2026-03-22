This is codes for OS lab `sh`.

Please add your information in conf/info.mk including SID, TOKEN, and server URL we provided

info.mk like this:
```makefile
# conf/info.mk
SID=123456789
TOKEN=abcdefghijklmn123456789
URL=http://123.456.789.0:9999
```
where `SID` is your student ID, `TOKEN` is the token we sent to your student email (student_id@smail.nju.edu.cn), and `URL` is the URL of the OJ server we provided.

View Server State
```bash
make server-state
```
If the OJ server is running, you should see a message like this:
```
Server is running.
```

Submit Code
```bash
make submit
```
If the submission is successful, you should see a message like this:
```
{
    "学号": "123456789",
    "姓名": "张三",
    "code submit inx": 1,
    "message": "Code file uploaded successfully, You should see the results after fifteen minutes"
}
```

Get Score
```bash
make score
```
If the score is available, you should see a message like this:
```
{
    "Score": "100.00%",
    "HighestScore": "100.00%",
    "Message": "Congratulations!",
    "code submit index": 1
}
```

Submit Report PDF
```bash
make report
```
