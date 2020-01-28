"""Calibration of starcam to the hipparcos / tycho star catalogue."""

from __future__ import print_function
from os import listdir, system, environ
from os.path import isfile, join
import cv2
import numpy as np
import sys
import time
from astropy.io import fits
from scipy import spatial

# Environment variables:
try:
    EXPOSURE_TIME = float(environ['EXPOSURE_TIME'])
except KeyError:
    EXPOSURE_TIME = 0.05  # s
try:
    APERTURE = float(environ['APERTURE'])
except KeyError:
    APERTURE = 60.7  # mm
try:
    DOUBLE_STAR_PX = float(environ['DOUBLE_STAR_PX'])
except KeyError:
    DOUBLE_STAR_PX = 3.5
    # pixels of seperation needed to distinguish stars from each other
try:
    POS_ERR_SIGMA = float(environ['POS_ERR_SIGMA'])
except KeyError:
    POS_ERR_SIGMA = 2
# Check all constellations which fall inside these bounds
# Note: Increasing this can actualy reduce the probability of finding a match
# as the true match has to stand out against a larger crowd


# NOTE: all of the following options multiply runtime by (N+2)^2
try:
    MAX_FALSE_STARS = int(environ['MAX_FALSE_STARS'])
except KeyError:
    MAX_FALSE_STARS = 2
    # max number of objects that can be brighter than the two brightest stars
if 'MAX_FALSE_STARS' in environ.keys():
    MAX_FALSE_STARS = int(environ['MAX_FALSE_STARS'])
else:
    MAX_FALSE_STARS = 2
try:
    DB_REDUNDANCY = int(environ['DB_REDUNDANCY'])
except KeyError:
    DB_REDUNDANCY = 1
    # of the brightest DB_REDUNDANCY+2 stars, we need at least 2
try:
    REQUIRED_STARS = int(environ['REQUIRED_STARS'])
except KeyError:
    REQUIRED_STARS = 5
    # How many stars should we try to match?
# For ultrawide fov this may be set to 3 for faster matching
# For ultranarrow fov, it may be necessary to set this to 5
# (Also send me an email and we'll talk)
# TODO: figure out how big "ultrawide" and "ultranarrow" are

HIPPARCOS_EPOC = 1991.25


def angles2xyz(ra, dec):
    """Angle convert RA to X Y Z in earth centered inertal frame."""
    x = np.cos(np.radians(ra)) * np.cos(np.radians(dec))
    y = np.sin(np.radians(ra)) * np.cos(np.radians(dec))
    z = np.sin(np.radians(dec))
    return list((x, y, z))


def getstardb(year=1991.25, filename="hip_main.dat"):
    """Load our star catalog, converting from id,ra,dec to x,y,z,id.

    Year in arguments is the current year frame, 1991.25 means no adjustment
    """
    yeardiff = year - HIPPARCOS_EPOC
    stardb = {}
    starfile = open(filename)
    for line in starfile.readlines():
        fields = line.split('|')
        try:
            hip_id = int(fields[1])  # Hiparcos ID
            mag = float(fields[5])  # Magnitude in Johnson V
            dec = yeardiff * float(fields[13]) / 3600000.0 + float(fields[9])
            cosdec = np.cos(np.pi * dec / 180.0)
            ra = (yeardiff * float(fields[12]) / (cosdec * 3600000.0) +
                  float(fields[8]))
            # x, y, z are earth centered inertial reference
            x = np.cos(np.pi * ra / 180.0) * cosdec
            y = np.sin(np.pi * ra / 180.0) * cosdec
            z = np.sin(np.pi * dec / 180.0)
        except ValueError:
            print("[DEBUG] Error reading star id %s". hip_id)
        try:
            f6 = int(fields[6])  # field 6 = Coarse variability flag
        except ValueError:
            f6 = 0
        if (int(fields[29]) == 0 or int(fields[29]) == 1) and f6 != 3:
            unreliable = 0  # 29 is Percentage of rejected data
        else:
            unreliable = 1
        stardb[hip_id] = [hip_id, mag, dec, ra, x, y, z, unreliable]
    return stardb


def basename(filename):
    """For some reasom this is to get part of the pathname."""
    if "." in filename:
        filename = ".".join(filename.split(".")[0:-1])
    return filename


# only do this part if we were run as a python script
if __name__ == '__main__':
    samplepath = sys.argv[1] + "/samples"
    image_names = [f for f in listdir(samplepath)
                   if isfile(join(samplepath, f))]
    print("[DEBUG] found files %s", image_names)
    num_images = len(image_names)
    print("[debug] caliberating off %s images", num_images)
    # NOTE: if you get NoneType error, delete non-png files
    images = np.asarray([cv2.imread(join(samplepath, image_names[n])).
                        astype(float) for n in range(0, num_images)])
    median_image = np.median(images, axis=0)
    cv2.imwrite(sys.argv[1] + "/median_image.png", median_image)
    system("md5sum " + samplepath + "/* >" + sys.argv[1] + "/checksum.txt")
    if system("diff -q " + sys.argv[1] + "/checksum.txt " + sys.argv[1] +
              "/calibration_data/checksum.txt") != 0:
        print ("Clearing old calibration data:")
        system("rm -rfv " + sys.argv[1] + "/calibration_data/* ")

    system("mv " + sys.argv[1] + "/checksum.txt " + sys.argv[1] +
           "/calibration_data/checksum.txt")
    year = int(time.strftime("%Y"))
    stardb = getstardb(year=year)

    astrometry_results = {}
    # filter background image for astrometry:important for starfield generator
    for n in range(0, num_images):
        images[n] -= median_image
        image_name = sys.argv[1] + "/calibration_data/" +\
            basename(image_names[n]) + ".png"
        img = np.clip(images[n], a_min=0, a_max=255).astype(np.uint8)
        cv2.imwrite(image_name, img)
        solve_cmd = "solve-field --skip-solved --downsample 4 " +\
            "--cpulimit 60 " + image_name
        print (solve_cmd)
        system(solve_cmd)
        if isfile(basename(image_name) + '.wcs'):
            cmd = ('wcsinfo ' + basename(image_name) +
                   '.wcs  | tr [:lower:] [:upper:] | tr " " "=" | grep ' +
                   '"=[0-9.-]*$" > ' + basename(image_name) + '.solved')
            print("[DEBUG] command: %s", cmd)
            system(cmd)
            hdulist = fits.open(basename(image_name) + ".corr")
            astrometry_results[image_names[n]] = \
                np.array(
                    [[i['flux'],
                      i['field_x'],
                      i['field_y'],
                      i['index_x'],
                      i['index_y']] + angles2xyz(i['index_ra'],
                     i['index_dec'])
                     for i in hdulist[1].data])

    # Use only values below the median for variance calculation.
    # This is equivalent to calculating variance after having filtered out
    # stars and background light
    THRESH_FACTOR = 5
    IMAGE_VARIANCE = np.ma.average(images**2, weights=images < 0)

    bestimage = ""
    maxstars = 0
    # for stars over 5*IMAGE_VARIANCE, find the corresponding star in the db
    sd = np.array(list(stardb.values()), dtype=object)
    # stardb had to explicitly convert to list for python3

    star_kd = spatial.cKDTree(sd[:, 4:7])
    for i in astrometry_results:
        astrometry_results[i] = \
            astrometry_results[i][astrometry_results[i][:, 0] >
                                  IMAGE_VARIANCE * THRESH_FACTOR]
        astrometry_results[i] = \
            np.hstack((sd[star_kd.query(astrometry_results[i][:, 5:8])[1]],
                       astrometry_results[i]))
        if len(astrometry_results[i]) > maxstars:
            bestimage = i
            maxstars = len(astrometry_results[i])
    astrometry_results_all = np.vstack(list(astrometry_results.values()))
    # Expicitly convert to a float array to prevent numpy error
    astrometry_results_all = astrometry_results_all.astype('float')

    # find the dimmest star
    dimmest_match = \
        astrometry_results_all[np.argmax(astrometry_results_all[:, 1]), :]

    BASE_FLUX = dimmest_match[8] / pow(10.0, -dimmest_match[1] / 2.5)
    print ("BASE_FLUX: ", BASE_FLUX)

    db_img_dist = np.linalg.norm(astrometry_results_all[:, 9:11] -
                                 astrometry_results_all[:, 11:13],
                                 axis=1)
    db_img_dist = db_img_dist - IMAGE_VARIANCE / (astrometry_results_all[:, 8])

    POS_VARIANCE = np.mean(db_img_dist)

    # <SB> execfile went away in python3
    # https://stackoverflow.com/questions/6357361/alternative-to-execfile-in-python-3
    filename = sys.argv[1] + "/calibration_data/" +\
        basename(bestimage) + ".solved"
    exec(compile(open(filename, "rb").read(), filename, 'exec'))

    # Emergency values for IMAGEW and IMAGEH because they aren't set anywhere
    IMAGEW, IMAGEH, PIXSCALE = (0, 0, 0)

    f_calib = open(sys.argv[1] + "/calibration.txt", 'w')
    f_calib.write("IMG_X=" + str(IMAGEW) + "\n")
    f_calib.write("IMG_Y=" + str(IMAGEH) + "\n")
    f_calib.write("PIXSCALE=" + str(PIXSCALE) + "\n")
    f_calib.write("DB_REDUNDANCY=" + str(DB_REDUNDANCY) + "\n")
    f_calib.write("DOUBLE_STAR_PX=" + str(DOUBLE_STAR_PX) + "\n")
    f_calib.write("REQUIRED_STARS=" + str(REQUIRED_STARS) + "\n")
    f_calib.write("MAX_FALSE_STARS=" + str(MAX_FALSE_STARS) + "\n")
    f_calib.write("BASE_FLUX=" + str(BASE_FLUX) + "\n")
    f_calib.write("THRESH_FACTOR=" + str(THRESH_FACTOR) + "\n")
    f_calib.write("IMAGE_VARIANCE=" + str(IMAGE_VARIANCE) + "\n")
    f_calib.write("POS_ERR_SIGMA=" + str(POS_ERR_SIGMA) + "\n")
    f_calib.write("POS_VARIANCE=" + str(POS_VARIANCE) + "\n")
    f_calib.write("APERTURE=" + str(APERTURE) + "\n")
    f_calib.write("EXPOSURE_TIME=" + str(EXPOSURE_TIME) + "\n")
    f_calib.close()

    print ("Calibration finished")
    print ("calibration.txt and median_image.png are in " + sys.argv[1] + "\n")
    system("cat " + sys.argv[1] + "/calibration.txt")
