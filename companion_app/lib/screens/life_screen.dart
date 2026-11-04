import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../providers/ble_provider.dart';
import '../providers/life_provider.dart';
import '../services/life_plan.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Life: plan healthy days, and see how they went. The watch nudges at the
/// right moments (water, stretch breaks, a breath, training, meals, a
/// check-in, winding down) and keeps score; this screen sets it all up.
class LifeScreen extends ConsumerWidget {
  const LifeScreen({super.key});

  static const water = Color(0xFF2EAFFF), stretch = Color(0xFFFFA500), mind = Color(0xFF5AC8FA);
  static const train = Color(0xFF9BE82A),
      meals = Color(0xFFFFCC00),
      social = Color(0xFFFA5577),
      wind = Color(0xFF7B80FF);

  static String hm(BuildContext context, int m) => TimeOfDay(hour: m ~/ 60 % 24, minute: m % 60).format(context);

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final st = ref.watch(lifeProvider);
    final p = st.plan;
    final n = ref.read(lifeProvider.notifier);
    final connected = ref.watch(bleProvider).isConnected;
    void set(void Function(Map<String, dynamic> j) f) => n.setPlan(p.edit(f));

    Future<void> pickTime(int cur, void Function(int) done) async {
      final t = await showTimePicker(
        context: context,
        initialTime: TimeOfDay(hour: cur ~/ 60, minute: cur % 60),
      );
      if (t != null) done(t.hour * 60 + t.minute);
    }

    final (b, l, d) = st.meals;
    return Scaffold(
      appBar: AppBar(title: const Text('Life')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 4, 16, 32),
        children: [
          _TodayCard(state: st),
          if (!connected)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text(
                'Changes go to the watch when it\'s connected.',
                style: TextStyle(color: AppColors.textDim, fontSize: 12.5),
              ),
            ),

          const SectionLabel('Your day'),
          Panel(
            child: Row(
              children: [
                const Icon(Icons.wb_twilight_rounded, color: AppColors.textDim),
                const SizedBox(width: 14),
                const Expanded(child: Text('Nudges between')),
                _TimeChip(hm(context, p.dayFrom), () => pickTime(p.dayFrom, (v) => set((j) => j['df'] = v))),
                const Padding(padding: EdgeInsets.symmetric(horizontal: 6), child: Text('and')),
                _TimeChip(hm(context, p.dayTo), () => pickTime(p.dayTo, (v) => set((j) => j['dt'] = v))),
              ],
            ),
          ),

          // ---- water
          _HabitCard(
            icon: Icons.water_drop_rounded,
            color: water,
            title: 'Drink water',
            subtitle: 'Glasses toward a goal, a nudge when it\'s been a while',
            on: p.water,
            onToggle: (v) => set((j) => j['w']['on'] = v ? 1 : 0),
            children: [
              _Stepper('Glasses a day', p.waterGoal, 4, 16, (v) => set((j) => j['w']['g'] = v)),
              _Choice('Remind every', p.waterEvery, const {
                45: '45 min',
                60: '1 h',
                90: '1.5 h',
                120: '2 h',
              }, (v) => set((j) => j['w']['ev'] = v)),
            ],
          ),
          // ---- stretch
          _HabitCard(
            icon: Icons.accessibility_new_rounded,
            color: stretch,
            title: 'Stretch breaks',
            subtitle: 'A guided 3-minute routine on the watch: neck, shoulders, back, wrists',
            on: p.stretch,
            onToggle: (v) => set((j) => j['s']['on'] = v ? 1 : 0),
            children: [
              _Stepper('Breaks a day', p.stretchGoal, 1, 8, (v) => set((j) => j['s']['g'] = v)),
              _Choice('Remind every', p.stretchEvery, const {
                60: '1 h',
                90: '1.5 h',
                120: '2 h',
                180: '3 h',
              }, (v) => set((j) => j['s']['ev'] = v)),
              _Row('Working hours', [
                _TimeChip(hm(context, p.workFrom), () => pickTime(p.workFrom, (v) => set((j) => j['s']['f'] = v))),
                const Text('  to  '),
                _TimeChip(hm(context, p.workTo), () => pickTime(p.workTo, (v) => set((j) => j['s']['t'] = v))),
              ]),
            ],
          ),
          // ---- mindful
          _HabitCard(
            icon: Icons.self_improvement_rounded,
            color: mind,
            title: 'Breathe & meditate',
            subtitle: 'Slow breathing with the watch: in as the circle grows, out as it shrinks',
            on: p.mind,
            onToggle: (v) => set((j) => j['m']['on'] = v ? 1 : 0),
            children: [
              _Stepper('Mindful minutes', p.mindGoal, 1, 30, (v) => set((j) => j['m']['g'] = v)),
              _Row('Nudge at', [
                _TimeChip(hm(context, p.mindAt), () => pickTime(p.mindAt, (v) => set((j) => j['m']['at'] = v))),
              ]),
            ],
          ),
          // ---- train
          _HabitCard(
            icon: Icons.fitness_center_rounded,
            color: train,
            title: 'Train',
            subtitle: 'On your days, at your time - until the Exercise ring has the minutes',
            on: p.train,
            onToggle: (v) => set((j) => j['tr']['on'] = v ? 1 : 0),
            children: [
              _Days(p.trainDays, (v) => set((j) => j['tr']['dy'] = v)),
              _Row('At', [
                _TimeChip(hm(context, p.trainAt), () => pickTime(p.trainAt, (v) => set((j) => j['tr']['at'] = v))),
              ]),
              _Choice('Minutes', p.trainGoal, const {
                20: '20',
                30: '30',
                45: '45',
                60: '60',
              }, (v) => set((j) => j['tr']['g'] = v)),
            ],
          ),
          // ---- meals
          _HabitCard(
            icon: Icons.restaurant_rounded,
            color: meals,
            title: 'Eat well',
            subtitle: 'Meal times with an idea for each - simple, balanced, mostly whole foods',
            on: p.meals,
            onToggle: (v) => set((j) => j['me']['on'] = v ? 1 : 0),
            children: [
              Wrap(
                spacing: 8,
                runSpacing: 4,
                children: [
                  for (final e in MealIdeas.diets.entries)
                    ChoiceChip(
                      label: Text(e.value),
                      selected: p.diet == e.key,
                      onSelected: (_) => set((j) => j['diet'] = e.key),
                    ),
                ],
              ),
              const SizedBox(height: 4),
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                value: p.aiMeals,
                onChanged: (v) => set((j) => j['ai'] = v ? 1 : 0),
                title: const Text('Ideas from the on-phone AI'),
                subtitle: Text(
                  st.writing
                      ? 'Writing today\'s ideas...'
                      : st.mealsByAi
                      ? 'Written for you today: your diet, notes, the season, training days'
                      : 'Uses the local LLM when one is installed (AI models); the built-in list otherwise',
                  style: const TextStyle(fontSize: 12.5),
                ),
              ),
              if (p.aiMeals)
                TextFormField(
                  initialValue: p.foodNotes,
                  maxLength: 120,
                  minLines: 1,
                  maxLines: 3,
                  decoration: const InputDecoration(
                    labelText: 'About your food (optional)',
                    hintText: 'Allergic to nuts, love fish, no spicy food, 15 minutes to cook...',
                  ),
                  onTapOutside: (_) => FocusManager.instance.primaryFocus?.unfocus(),
                  onChanged: (v) => _debounce(() => set((j) => j['notes'] = v.trim())),
                ),
              const SizedBox(height: 8),
              _Row('Breakfast', [
                _TimeChip(
                  hm(context, p.breakfastAt),
                  () => pickTime(p.breakfastAt, (v) => set((j) => j['me']['b'] = v)),
                ),
              ]),
              _Row('Lunch', [
                _TimeChip(hm(context, p.lunchAt), () => pickTime(p.lunchAt, (v) => set((j) => j['me']['l'] = v))),
              ]),
              _Row('Dinner', [
                _TimeChip(hm(context, p.dinnerAt), () => pickTime(p.dinnerAt, (v) => set((j) => j['me']['d'] = v))),
              ]),
              const SizedBox(height: 6),
              Row(
                children: [
                  const Expanded(
                    child: Text(
                      'Today\'s ideas',
                      style: TextStyle(color: AppColors.textDim, fontWeight: FontWeight.w600),
                    ),
                  ),
                  if (st.writing)
                    const Padding(
                      padding: EdgeInsets.all(12),
                      child: SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2)),
                    )
                  else
                    TextButton.icon(
                      onPressed: n.shuffleMeals,
                      icon: Icon(st.mealsByAi ? Icons.auto_awesome_rounded : Icons.shuffle_rounded, size: 18),
                      label: const Text('Others'),
                    ),
                ],
              ),
              for (final (k, v) in [('Breakfast', b), ('Lunch', l), ('Dinner', d)])
                Padding(
                  padding: const EdgeInsets.only(bottom: 6),
                  child: Text.rich(
                    TextSpan(
                      children: [
                        TextSpan(
                          text: '$k  ',
                          style: const TextStyle(color: meals, fontWeight: FontWeight.w600),
                        ),
                        TextSpan(text: v),
                      ],
                    ),
                  ),
                ),
              const Text(
                'Ideas, not a diet plan: for medical or special needs, ask a dietitian.',
                style: TextStyle(color: AppColors.textFaint, fontSize: 11.5),
              ),
            ],
          ),
          // ---- social
          _HabitCard(
            icon: Icons.people_alt_rounded,
            color: social,
            title: 'Stay in touch',
            subtitle: 'A nudge to call, text or meet someone - connection is good for you',
            on: p.social,
            onToggle: (v) => set((j) => j['so']['on'] = v ? 1 : 0),
            children: [
              TextFormField(
                initialValue: p.socialWho,
                maxLength: 22,
                decoration: const InputDecoration(labelText: 'Who (optional)', hintText: 'Mum, Marco, a friend...'),
                onFieldSubmitted: (v) => set((j) => j['so']['who'] = v.trim()),
                onTapOutside: (_) => FocusManager.instance.primaryFocus?.unfocus(),
                onChanged: (v) => _debounce(() => set((j) => j['so']['who'] = v.trim())),
              ),
              _Days(p.socialDays, (v) => set((j) => j['so']['dy'] = v)),
              _Row('At', [
                _TimeChip(hm(context, p.socialAt), () => pickTime(p.socialAt, (v) => set((j) => j['so']['at'] = v))),
              ]),
            ],
          ),
          // ---- wind down
          _HabitCard(
            icon: Icons.bedtime_rounded,
            color: wind,
            title: 'Wind down',
            subtitle: 'Before bed: lights low, screens off - the watch can turn on Do Not Disturb',
            on: p.wind,
            onToggle: (v) => set((j) => j['wd']['on'] = v ? 1 : 0),
            children: [
              _Row('At', [
                _TimeChip(hm(context, p.windAt), () => pickTime(p.windAt, (v) => set((j) => j['wd']['at'] = v))),
              ]),
            ],
          ),
          const SizedBox(height: 12),
          const Text(
            'On the watch: the Life app shows today\'s score and each habit; nudges come as a card with "Do it" or '
            '"Later" (15 minutes), never in Do Not Disturb or during a workout. The score is the habits done out of the '
            'ones set up for the day; 60% or more keeps the streak going.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}

DateTime? _lastEdit;
void _debounce(void Function() f) {
  final at = _lastEdit = DateTime.now();
  Future.delayed(const Duration(milliseconds: 900), () {
    if (_lastEdit == at) f();
  });
}

/// Today's score and the last week.
class _TodayCard extends StatelessWidget {
  final LifeState state;
  const _TodayCard({required this.state});

  @override
  Widget build(BuildContext context) {
    final t = state.today;
    final now = DateTime.now();
    final byDate = {for (final d in state.days) DateTime(d.date.year, d.date.month, d.date.day): d};
    const names = ['M', 'T', 'W', 'T', 'F', 'S', 'S'];
    return Panel(
      padding: const EdgeInsets.fromLTRB(18, 18, 18, 14),
      child: Column(
        children: [
          Row(
            children: [
              Ring(
                value: (t?.score ?? 0) / 100,
                size: 96,
                stroke: 10,
                color: const Color(0xFF2CCA70),
                child: Text('${t?.score ?? 0}%', style: const TextStyle(fontSize: 22, fontWeight: FontWeight.w700)),
              ),
              const SizedBox(width: 18),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      t == null ? 'Today' : 'Today so far',
                      style: const TextStyle(fontSize: 17, fontWeight: FontWeight.w600),
                    ),
                    const SizedBox(height: 6),
                    if (t == null)
                      const Text(
                        'The watch sends how the day goes once it\'s connected.',
                        style: TextStyle(color: AppColors.textDim, fontSize: 13),
                      )
                    else
                      Wrap(
                        spacing: 10,
                        runSpacing: 4,
                        children: [
                          _mini(Icons.water_drop_rounded, LifeScreen.water, '${t.water}'),
                          _mini(Icons.accessibility_new_rounded, LifeScreen.stretch, '${t.stretch}'),
                          _mini(Icons.self_improvement_rounded, LifeScreen.mind, '${t.mind}m'),
                          _mini(Icons.fitness_center_rounded, LifeScreen.train, '${t.train}m'),
                          _mini(Icons.restaurant_rounded, LifeScreen.meals, '${t.meals}/3'),
                        ],
                      ),
                    const SizedBox(height: 6),
                    Text(
                      state.streak > 0 ? '${state.streak}-day streak' : 'Start a streak today',
                      style: const TextStyle(color: Color(0xFF2CCA70), fontWeight: FontWeight.w600),
                    ),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              for (var i = 6; i >= 0; i--)
                Expanded(
                  child: Builder(
                    builder: (_) {
                      final day = DateTime(now.year, now.month, now.day).subtract(Duration(days: i));
                      final sc = i == 0 ? (t?.score ?? 0) : (byDate[day]?.score ?? 0);
                      return Column(
                        children: [
                          SizedBox(
                            height: 46,
                            child: Align(
                              alignment: Alignment.bottomCenter,
                              child: Container(
                                width: 14,
                                height: math.max(4, 46 * sc / 100),
                                decoration: BoxDecoration(
                                  color: sc >= 60 ? const Color(0xFF2CCA70) : AppColors.surfaceHigh,
                                  borderRadius: BorderRadius.circular(7),
                                ),
                              ),
                            ),
                          ),
                          const SizedBox(height: 6),
                          Text(
                            names[day.weekday - 1],
                            style: TextStyle(color: i == 0 ? AppColors.text : AppColors.textDim, fontSize: 12),
                          ),
                        ],
                      );
                    },
                  ),
                ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _mini(IconData i, Color c, String v) => Row(
    mainAxisSize: MainAxisSize.min,
    children: [
      Icon(i, size: 16, color: c),
      const SizedBox(width: 3),
      Text(v, style: const TextStyle(fontSize: 13)),
    ],
  );
}

class _HabitCard extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String title, subtitle;
  final bool on;
  final ValueChanged<bool> onToggle;
  final List<Widget> children;
  const _HabitCard({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.on,
    required this.onToggle,
    required this.children,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 12),
      child: Panel(
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                IconChip(icon, color: color),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(title, style: const TextStyle(fontSize: 15.5, fontWeight: FontWeight.w600)),
                      Text(subtitle, style: const TextStyle(color: AppColors.textDim, fontSize: 12.5)),
                    ],
                  ),
                ),
                Switch(value: on, onChanged: onToggle),
              ],
            ),
            if (on) ...[const SizedBox(height: 8), ...children],
          ],
        ),
      ),
    );
  }
}

class _Row extends StatelessWidget {
  final String label;
  final List<Widget> trailing;
  const _Row(this.label, this.trailing);
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 4),
    child: Row(
      children: [
        Expanded(child: Text(label)),
        ...trailing,
      ],
    ),
  );
}

class _TimeChip extends StatelessWidget {
  final String text;
  final VoidCallback onTap;
  const _TimeChip(this.text, this.onTap);
  @override
  Widget build(BuildContext context) => ActionChip(label: Text(text), onPressed: onTap);
}

class _Stepper extends StatelessWidget {
  final String label;
  final int value, min, max;
  final ValueChanged<int> onChanged;
  const _Stepper(this.label, this.value, this.min, this.max, this.onChanged);
  @override
  Widget build(BuildContext context) => _Row(label, [
    IconButton(
      onPressed: value > min ? () => onChanged(value - 1) : null,
      icon: const Icon(Icons.remove_circle_outline),
    ),
    SizedBox(
      width: 28,
      child: Text('$value', textAlign: TextAlign.center, style: const TextStyle(fontSize: 16)),
    ),
    IconButton(onPressed: value < max ? () => onChanged(value + 1) : null, icon: const Icon(Icons.add_circle_outline)),
  ]);
}

class _Choice extends StatelessWidget {
  final String label;
  final int value;
  final Map<int, String> options;
  final ValueChanged<int> onChanged;
  const _Choice(this.label, this.value, this.options, this.onChanged);
  @override
  Widget build(BuildContext context) => _Row(label, [
    DropdownButton<int>(
      value: options.containsKey(value) ? value : options.keys.first,
      underline: const SizedBox.shrink(),
      items: [for (final e in options.entries) DropdownMenuItem(value: e.key, child: Text(e.value))],
      onChanged: (v) => v == null ? null : onChanged(v),
    ),
  ]);
}

/// Weekday chips, Monday first; bit 0 of [days] is Sunday (as the watch counts).
class _Days extends StatelessWidget {
  final int days;
  final ValueChanged<int> onChanged;
  const _Days(this.days, this.onChanged);
  static const _l = ['M', 'T', 'W', 'T', 'F', 'S', 'S'];
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 4),
    child: Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        for (var i = 0; i < 7; i++)
          Builder(
            builder: (_) {
              final bit = 1 << ((i + 1) % 7);
              final on = days & bit != 0;
              return GestureDetector(
                onTap: () => onChanged(days ^ bit),
                child: CircleAvatar(
                  radius: 17,
                  backgroundColor: on ? AppColors.accent : AppColors.surfaceHigh,
                  child: Text(_l[i], style: TextStyle(color: on ? Colors.white : AppColors.textDim, fontSize: 13)),
                ),
              );
            },
          ),
      ],
    ),
  );
}
