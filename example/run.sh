pip install -r requirements.txt

uwsgi --build-plugin https://github.com/Datadog/uwsgi-dogstatsd

uwsgi app.ini

