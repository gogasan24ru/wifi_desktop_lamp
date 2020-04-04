gzip -k *.html
gzip -k *.css
gzip -k *.js
gzip -k *.png
#							 ur lamp IP
#TODO maybe auth?
for file in `ls -A1 *.gz`; do curl -F "file=@$PWD/$file" 192.168.87.10/edit; done
rm *.gz
