# HOME_XYZ: home X, Y and Z in one move, each axis at the same speed.
#
# Klipper's cartesian G28 homes one axis after another. This uses the
# delta-style path instead: one homing move with all three endstops, each
# endstop stopping only its own motor. Every axis travels the same distance,
# so every axis moves at SPEED (default: Z's homing_speed, the slowest axis).
#
# Copy to klippy/extras/ and add [home_xyz] to printer.cfg.
import math
from . import homing

class _Info:
    def __init__(self, rail, speed, second_speed):
        self._rail = rail
        self._speed = speed
        self._second_speed = second_speed
    def get_homing_info(self):
        hi = self._rail.get_homing_info()
        k = math.sqrt(3.)
        return hi._replace(speed=self._speed * k,
                           second_homing_speed=self._second_speed * k,
                           retract_speed=self._speed * k,
                           retract_dist=hi.retract_dist * k)
    def __getattr__(self, name):
        return getattr(self._rail, name)

class HomeXYZ:
    def __init__(self, config):
        self.printer = config.get_printer()
        zc = config.getsection('stepper_z')
        self.speed = config.getfloat('speed', zc.getfloat('homing_speed', 5.),
                                     above=0.)
        self.second_speed = config.getfloat(
            'second_speed', zc.getfloat('second_homing_speed', self.speed / 2.),
            above=0.)
        self.printer.lookup_object('gcode').register_command(
            'HOME_XYZ', self.cmd_HOME_XYZ,
            desc="Home X, Y and Z together, every axis at the same speed")
    def cmd_HOME_XYZ(self, gcmd):
        kin = self.printer.lookup_object('toolhead').get_kinematics()
        rails = kin.rails[:3]
        homepos = [None] * 4
        dist = 0.
        for axis, rail in enumerate(rails):
            lo, hi = rail.get_range()
            info = rail.get_homing_info()
            homepos[axis] = info.position_endstop
            dist = max(dist, 1.5 * (hi - lo))
        forcepos = list(homepos)
        for axis, rail in enumerate(rails):
            if rail.get_homing_info().positive_dir:
                forcepos[axis] -= dist
            else:
                forcepos[axis] += dist
        state = homing.Homing(self.printer)
        state.set_axes([0, 1, 2])
        rails = [_Info(rails[0], self.speed, self.second_speed)] + rails[1:]
        try:
            state.home_rails(rails, forcepos, homepos)
        except self.printer.command_error:
            if self.printer.is_shutdown():
                raise self.printer.command_error(
                    "Homing failed due to printer shutdown")
            self.printer.lookup_object('stepper_enable').motor_off()
            raise

def load_config(config):
    return HomeXYZ(config)
