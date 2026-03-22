# Please do not modify Makefile and lab.mk
-include conf/lab.mk

# Please add your information in conf/info.mk including SID, TOKEN, and server URL we provided
-include conf/info.mk
# info.mk like this:
# SID=123456789
# TOKEN=abcdefghijklmn123456789
# URL=http://123.456.789.0:9999


sh: sh.c
	gcc sh.c -o sh

clean:
	rm -f *.o sh


STYLE=\033[1;31m
NC=\033[0m

info-check:
	@if test -z "$(SID)"; then \
		echo "${STYLE}Please set SID in conf/info.mk${NC}"; \
		false; \
	fi
	@if test -z "`echo $(SID) | grep '^[0-9]\{9\}$$'`"; then \
		echo -n "${STYLE}Your SID (${SID}) does not appear to be correct. Continue? [y/N]${NC} "; \
		read -p "" r; \
		test "$$r" = y; \
	fi
	@if test -z "$(TOKEN)"; then \
		echo "${STYLE}Please set TOKEN in conf/info.mk${NC}"; \
		false; \
	fi

server-state:
	curl "${URL}/?token=${TOKEN}"

submit: info-check
	curl -F "token=${TOKEN}" -F "lab_num=${LAB_NUM}" -F "file=@sh.c" ${URL}/upload_code

report: info-check
	@if ! test -f $(SID).pdf; then \
		echo "${STYLE}Please put your report in a file named $(SID).pdf${NC}"; \
		false; \
	fi
	curl -F "token=${TOKEN}" -F "lab_num=${LAB_NUM}" -F "file=@${SID}.pdf" ${URL}/upload_report

score: info-check
	curl "${URL}/download?token=${TOKEN}&lab_num=${LAB_NUM}"

.PHONY: clean info-check submit report score
