from django.db import models

class JobQueue(models.Model):
    name = models.CharField(max_length=32, unique=True, primary_key=True)

class QueuedJob(models.Model):
    q = models.ForeignKey(JobQueue, related_name='jobs')
    priority = models.SmallIntegerField(blank=True, default=0)
    jobid = models.CharField(max_length=32)
    stopwork = models.BooleanField(blank=True, default=False)
    enqueuetime = models.DateTimeField(blank=True, default='2000-01-01')
    # work completed so far...

    def __str__(self):
        return 'QueuedJob: %s' % self.jobid

class Worker(models.Model):
    hostname = models.CharField(max_length=256)
    ip = models.IPAddressField()
    job = models.ForeignKey(QueuedJob, related_name='workers', blank=True, null=True)

    def pretty_index_list(self):
        return ', '.join(['%i'%i.indexid + (i.healpix > -1 and '-%i'%i.healpix or '')
                          for i in self.indexes.all()])

class LoadedIndex(models.Model):
    indexid = models.IntegerField()
    healpix = models.IntegerField()
    healpix_nside = models.IntegerField()
    worker = models.ForeignKey(Worker, related_name='indexes')



